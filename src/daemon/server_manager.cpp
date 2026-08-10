#include <minitun/daemon/server_manager.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <asio/associated_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <openssl/ssl.h>

#include <minitun/common/error.hpp>
#include <minitun/common/failpoint.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/daemon/credential_keys.hpp>
#include <minitun/daemon/reconnect_backoff.hpp>
#include <minitun/daemon/tunnel_reconciler.hpp>
#include <minitun/daemon/worker_pool.hpp>
#include <minitun/protocol/auth.hpp>
#include <minitun/protocol/messages.hpp>
#include <minitun/protocol/state_machine.hpp>
#include <minitun/protocol/tls.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

namespace minitun::daemon {
namespace {

inline constexpr auto kMaximumReconcileInterval = std::chrono::seconds{10};
inline constexpr auto kMaximumConnectTimeout = std::chrono::minutes{5};
inline constexpr auto kMaximumRelayTimeout = std::chrono::hours{24};
inline constexpr std::size_t kTunnelSyncWindow = 32U;

struct RuntimeMetrics final {
    std::atomic<std::uint64_t> reconnects{0U};
    std::atomic<std::uint64_t> quota_rejections{0U};
    std::atomic<std::uint64_t> persistence_errors{0U};
    std::atomic<std::uint64_t> protocol_errors{0U};
    std::atomic<std::uint64_t> bytes_in{0U};
    std::atomic<std::uint64_t> bytes_out{0U};
    std::atomic<std::uint64_t> tls_resumptions{0U};
};

[[nodiscard]] common::Result<void> validate_options(const ServerManagerOptions& options) {
    if (options.reconcile_interval < std::chrono::milliseconds{50} ||
        options.reconcile_interval > kMaximumReconcileInterval ||
        options.connect_timeout <= std::chrono::seconds::zero() ||
        options.connect_timeout > kMaximumConnectTimeout ||
        options.handshake_timeout <= std::chrono::seconds::zero() ||
        options.handshake_timeout > kMaximumConnectTimeout ||
        options.relay_inactivity_timeout <= std::chrono::seconds::zero() ||
        options.relay_inactivity_timeout > kMaximumRelayTimeout ||
        options.graceful_shutdown_timeout <= std::chrono::seconds::zero() ||
        options.graceful_shutdown_timeout > kMaximumConnectTimeout ||
        options.max_idle_workers_per_server == 0U || options.max_idle_workers_per_server > 128U ||
        options.max_total_idle_workers == 0U || options.max_total_idle_workers > 4'096U ||
        options.max_total_connections == 0U || options.max_total_connections > 100'000U ||
        options.max_total_connections < options.max_total_idle_workers) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "remote session timeout configuration is invalid");
    }
    return common::Result<void>::success();
}

[[nodiscard]] std::string credential_key(const storage::ServerRecord& server) {
    if (server.credential_ref.has_value()) {
        return *server.credential_ref;
    }
    return {};
}

} // namespace

class ServerManager::Impl final : public std::enable_shared_from_this<ServerManager::Impl> {
  private:
    class ServerSession final : public std::enable_shared_from_this<ServerSession> {
      public:
        enum class TerminalState : std::uint8_t {
            running,
            authentication_failed,
            stopped,
        };

        ServerSession(asio::io_context& io_context, storage::StateRepository& repository,
                      storage::CredentialStore& credentials, common::Id client_id,
                      storage::ServerRecord server, std::shared_ptr<asio::ssl::context> tls_context,
                      std::shared_ptr<WorkerBudget> worker_budget,
                      std::shared_ptr<WorkerBudget> connection_budget,
                      std::shared_ptr<TunnelReconciler> tunnel_reconciler,
                      std::shared_ptr<RuntimeMetrics> metrics,
                      std::shared_ptr<asio::thread_pool> db_pool, ServerManagerOptions options)
            : repository_(repository), credentials_(credentials), client_id_(std::move(client_id)),
              server_(std::move(server)), remote_endpoint_text_(server_.endpoint.to_string()),
              config_revision_(server_.config_revision),
              tls_context_(std::move(tls_context)),
              tls_session_cache_(std::make_shared<protocol::TlsSessionCache>()),
              worker_budget_(std::move(worker_budget)),
              connection_budget_(std::move(connection_budget)),
              tunnel_reconciler_(std::move(tunnel_reconciler)), metrics_(std::move(metrics)),
              db_pool_(std::move(db_pool)), options_(std::move(options)),
              strand_(asio::make_strand(io_context)), resolver_(strand_), reconnect_timer_(strand_),
              idle_timer_(strand_), operation_timer_(strand_) {}

        ~ServerSession() {
            stop_worker_pool();
            close_transport();
        }

        void start() {
            auto self = shared_from_this();
            asio::co_spawn(strand_, run(), [self](const std::exception_ptr& failure) {
                if (failure) {
                    self->terminal_state_.store(TerminalState::stopped);
                    asio::co_spawn(
                        self->strand_,
                        self->persist_state(storage::ServerActualState::error,
                                            {common::ErrorCode::internal_error,
                                             "remote session failed unexpectedly"},
                                            self->backoff_.attempt()),
                        [self](const std::exception_ptr&, const common::Result<void>&) {});
                    common::log_error("remote server session ended with an exception",
                                      self->log_context(common::ErrorCode::internal_error));
                }
                self->cancel_timers();
                self->stop_worker_pool();
                if (!self->goaway_in_progress_) {
                    self->close_transport();
                }
            });
        }

        void stop() {
            auto self = shared_from_this();
            asio::dispatch(strand_, [self] { self->begin_stop(); });
        }

        void notify_changed() {
            auto self = shared_from_this();
            asio::dispatch(strand_, [self] {
                if (self->stopping_) {
                    return;
                }
                self->reconcile_requested_ = true;
                if (self->idle_wait_active_) {
                    try {
                        static_cast<void>(self->idle_timer_.cancel());
                    } catch (...) {
                    }
                }
                // Keep an offline session's existing reconnect deadline. A burst
                // of tunnel mutations must not turn each notification into a
                // failed connection attempt and drive the backoff to its maximum.
            });
        }

        void supersede() {
            auto invalidated = tunnel_reconciler_->invalidate(server_.id);
            if (!invalidated) {
                metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                common::log_error("failed to invalidate superseded tunnel generation",
                                  log_context(invalidated.error().code()));
            }
            {
                const std::scoped_lock lock{persistence_mutex_};
                persistence_allowed_ = false;
            }
            stop();
        }

        [[nodiscard]] bool matches(const storage::ServerRecord& server) const {
            return server_.endpoint == server.endpoint &&
                   server_.credential_ref == server.credential_ref &&
                   server_.tls_server_name == server.tls_server_name &&
                   server_.ca_credential_ref == server.ca_credential_ref &&
                   server_.client_certificate_ref == server.client_certificate_ref &&
                   server_.client_private_key_ref == server.client_private_key_ref;
        }

        void refresh_config_revision(const std::uint64_t revision) noexcept {
            config_revision_.store(revision, std::memory_order_release);
        }

        [[nodiscard]] TerminalState terminal_state() const noexcept {
            return terminal_state_.load();
        }

      private:
        template <typename Operation>
        [[nodiscard]] asio::awaitable<std::invoke_result_t<Operation&>>
        run_db(Operation operation) {
            using ReturnT = std::invoke_result_t<Operation&>;
            auto db_pool = db_pool_;
            auto operation_state = std::make_shared<Operation>(std::move(operation));
            return asio::async_initiate<decltype(asio::use_awaitable), void(ReturnT)>(
                [db_pool, operation_state](auto handler) mutable {
                    auto completion_executor = asio::get_associated_executor(handler);
                    asio::post(db_pool->get_executor(),
                               [operation_state, handler = std::move(handler),
                                completion_executor = std::move(completion_executor)]() mutable {
                                   std::shared_ptr<ReturnT> result;
                                   try {
                                       result =
                                           std::make_shared<ReturnT>(std::invoke(*operation_state));
                                   } catch (...) {
                                       result = std::make_shared<ReturnT>(
                                           ReturnT::failure(common::ErrorCode::internal_error,
                                                            "database operation failed"));
                                   }
                                   asio::post(std::move(completion_executor),
                                              [handler = std::move(handler),
                                               result = std::move(result)]() mutable {
                                                  handler(std::move(*result));
                                              });
                               });
                },
                asio::use_awaitable);
        }

        enum class AttemptKind : std::uint8_t {
            disconnected,
            authentication_failed,
            stopped,
        };

        struct AttemptResult final {
            AttemptKind kind{AttemptKind::disconnected};
            common::Error error{common::ErrorCode::connection_failed, "remote server disconnected"};
        };

        // GCC 12 may duplicate destruction of owning temporaries in a co_await
        // call expression. Copy views into owning DB tasks only in non-coroutine helpers.
        struct PersistenceErrorView final {
            std::optional<common::ErrorCode> code;
            std::string_view message;
        };

        [[nodiscard]] asio::awaitable<void> run() {
            while (!stopping_) {
                // GCC 12 duplicates destruction of owning temporaries created
                // inline inside a co_await call expression. Keep the operation
                // in a named local so its owning captures are destroyed once.
                auto begin_generation_operation =
                    [reconciler = tunnel_reconciler_, server_id = server_.id] {
                        return reconciler->begin_generation(server_id);
                    };
                auto generation =
                    co_await run_db(std::move(begin_generation_operation));
                if (!generation) {
                    terminal_state_.store(TerminalState::stopped);
                    metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                    co_return;
                }
                attempt_generation_ = *generation;
                terminal_state_.store(TerminalState::running);
                const AttemptResult result = co_await run_attempt();
                stop_worker_pool();
                close_transport();
                session_generation_ = 0U;
                static_cast<void>(co_await mark_tunnels_pending(
                    stopping_ ? PersistenceErrorView{}
                              : PersistenceErrorView{result.error.code(),
                                                     result.error.message()}));
                if (stopping_ || result.kind == AttemptKind::stopped) {
                    attempt_generation_ = 0U;
                    terminal_state_.store(TerminalState::stopped);
                    co_return;
                }
                if (result.kind == AttemptKind::authentication_failed) {
                    attempt_generation_ = 0U;
                    terminal_state_.store(TerminalState::authentication_failed);
                    static_cast<void>(co_await persist_state(
                        storage::ServerActualState::not_authenticated,
                        {result.error.code(), result.error.message()}, backoff_.attempt()));
                    co_return;
                }

                const auto delay = backoff_.next_delay();
                attempt_generation_ = 0U;
                metrics_->reconnects.fetch_add(1U, std::memory_order_relaxed);
                static_cast<void>(co_await persist_state(
                    storage::ServerActualState::backoff,
                    {result.error.code(), result.error.message()}, backoff_.attempt()));
                common::log_warn("remote server connection entered backoff",
                                 log_context(result.error.code()));
                reconnect_timer_.expires_after(delay);
                asio::error_code timer_error;
                co_await reconnect_timer_.async_wait(
                    asio::redirect_error(asio::use_awaitable, timer_error));
                if (timer_error || stopping_) {
                    terminal_state_.store(TerminalState::stopped);
                    co_return;
                }
            }
        }

        [[nodiscard]] asio::awaitable<AttemptResult> run_attempt() {
            const auto attempt_started = std::chrono::steady_clock::now();
            pending_reconcile_responses_.clear();
            protocol::StateMachine state{protocol::PeerRole::client,
                                         protocol::ConnectionKind::control};
            stream_ = std::make_unique<protocol::TlsStream>(strand_, *tls_context_);
            static_cast<void>(co_await persist_state(storage::ServerActualState::connecting, {},
                                                     backoff_.attempt()));

            arm_operation_timeout(options_.connect_timeout);
            asio::error_code error;
            auto endpoints = co_await resolver_.async_resolve(
                server_.endpoint.host(), std::to_string(server_.endpoint.port()),
                asio::redirect_error(asio::use_awaitable, error));
            if (!error) {
                co_await asio::async_connect(stream_->lowest_layer(), endpoints,
                                             asio::redirect_error(asio::use_awaitable, error));
            }
            cancel_operation_timeout();
            if (error) {
                co_return disconnected(common::ErrorCode::connection_failed,
                                       "remote TCP connection failed");
            }
            protocol::configure_tcp_transport(stream_->lowest_layer());

            auto configured = protocol::configure_client_tls_stream(
                *stream_, server_.tls_server_name.value_or(server_.endpoint.host()),
                options_.insecure_skip_verify);
            if (!configured) {
                co_return disconnected(configured.error().code(), configured.error().message());
            }
            static_cast<void>(tls_session_cache_->restore(*stream_));
            static_cast<void>(co_await persist_state(storage::ServerActualState::tls_handshake, {},
                                                     backoff_.attempt()));
            arm_operation_timeout(options_.handshake_timeout);
            co_await stream_->async_handshake(asio::ssl::stream_base::client,
                                              asio::redirect_error(asio::use_awaitable, error));
            cancel_operation_timeout();
            if (error) {
                co_return disconnected(common::ErrorCode::tls_error,
                                       "TLS peer verification or handshake failed");
            }
            if (protocol::tls_session_reused(*stream_)) {
                metrics_->tls_resumptions.fetch_add(1U, std::memory_order_relaxed);
            }

            static_cast<void>(co_await persist_state(storage::ServerActualState::authenticating, {},
                                                     backoff_.attempt()));
            auto token =
                co_await run_db([this] { return credentials_.get(credential_key(server_)); });
            if (!token) {
                co_return authentication_failed(common::ErrorCode::not_authenticated,
                                                "server credential is unavailable");
            }

            const protocol::CapabilitySet offered_capabilities =
                protocol::kSupportedCapabilities;
            auto hello_payload = protocol::encode_hello(
                {client_id_.str(), offered_capabilities});
            if (!hello_payload) {
                co_return disconnected(common::ErrorCode::internal_error, "failed to encode HELLO");
            }
            const protocol::Frame hello_frame{protocol::MessageType::hello, 0U, 1U,
                                              std::move(*hello_payload)};
            if (auto written = co_await write_frame(state, hello_frame, options_.handshake_timeout);
                !written) {
                co_return disconnected(written.error().code(), written.error().message());
            }

            auto ack_frame = co_await read_frame(state, options_.handshake_timeout);
            if (!ack_frame || ack_frame->type != protocol::MessageType::hello_ack) {
                co_return disconnected(common::ErrorCode::protocol_error,
                                       "remote HELLO_ACK is invalid");
            }
            auto ack = protocol::decode_hello_ack(ack_frame->payload);
            if (!ack) {
                co_return disconnected(ack.error().code(), ack.error().message());
            }
            if ((ack->selected_capabilities & offered_capabilities) !=
                ack->selected_capabilities) {
                co_return disconnected(common::ErrorCode::protocol_error,
                                       "remote server selected an unoffered capability");
            }

            const std::int64_t timestamp = common::unix_seconds_now();
            auto digest = protocol::compute_authentication_data(
                token->view(), client_id_.str(), ack->server_id, timestamp, ack->nonce,
                ack->selected_capabilities);
            if (!digest) {
                co_return disconnected(digest.error().code(), digest.error().message());
            }
            auto auth_payload = protocol::encode_auth(
                {client_id_.str(), timestamp, ack->nonce, *digest,
                 ack->selected_capabilities});
            if (!auth_payload) {
                co_return disconnected(common::ErrorCode::internal_error, "failed to encode AUTH");
            }
            const protocol::Frame auth_request_frame{protocol::MessageType::auth, 0U, 2U,
                                                     std::move(*auth_payload)};
            if (auto written =
                    co_await write_frame(state, auth_request_frame, options_.handshake_timeout);
                !written) {
                co_return disconnected(written.error().code(), written.error().message());
            }

            auto auth_frame = co_await read_frame(state, options_.handshake_timeout);
            if (!auth_frame) {
                co_return disconnected(auth_frame.error().code(), auth_frame.error().message());
            }
            if (auth_frame->type == protocol::MessageType::auth_error) {
                static_cast<void>(protocol::decode_auth_error(auth_frame->payload));
                co_return authentication_failed(common::ErrorCode::authentication_failed,
                                                "remote authentication failed");
            }
            if (auth_frame->type != protocol::MessageType::auth_ok) {
                co_return disconnected(common::ErrorCode::protocol_error,
                                       "remote authentication response is invalid");
            }
            auto auth_ok = protocol::decode_auth_ok(auth_frame->payload);
            if (!auth_ok) {
                co_return disconnected(auth_ok.error().code(), auth_ok.error().message());
            }
            static_cast<void>(tls_session_cache_->capture(*stream_));

            session_generation_ = auth_ok->session_generation;
            remote_server_id_ = ack->server_id;
            auto session_psk =
                std::make_shared<const common::SecureString>(std::move(*token));
            const std::uint16_t max_idle_workers = static_cast<std::uint16_t>(std::min<std::size_t>(
                auth_ok->max_idle_workers, options_.max_idle_workers_per_server));
            const std::uint16_t min_idle_workers =
                std::min(auth_ok->min_idle_workers, max_idle_workers);
            auto workers = WorkerPool::create(
                strand_, tls_context_, worker_budget_, connection_budget_,
                {
                    .endpoint = server_.endpoint,
                    .tls_server_name = server_.tls_server_name.value_or(server_.endpoint.host()),
                    .server_id = server_.id.str(),
                    .remote_server_id = remote_server_id_,
                    .client_id = client_id_.str(),
                    .psk = std::move(session_psk),
                    .tls_session_cache = tls_session_cache_,
                    .session_generation = session_generation_,
                    .min_idle_workers = min_idle_workers,
                    .max_idle_workers = max_idle_workers,
                    .connect_timeout = options_.connect_timeout,
                    .handshake_timeout = options_.handshake_timeout,
                    .idle_timeout =
                        std::chrono::seconds{protocol::kMaximumWorkerIdleTimeoutSeconds +
                                             protocol::kWorkerIdleTimeoutGraceSeconds},
                    .relay_inactivity_timeout = options_.relay_inactivity_timeout,
                    .graceful_shutdown_timeout = options_.graceful_shutdown_timeout,
                    .insecure_skip_verify = options_.insecure_skip_verify,
                    .quota_rejection_handler =
                        [metrics = metrics_] {
                            metrics->quota_rejections.fetch_add(1U, std::memory_order_relaxed);
                        },
                    .tls_resumption_handler =
                        [metrics = metrics_] {
                            metrics->tls_resumptions.fetch_add(1U, std::memory_order_relaxed);
                        },
                    .relay_stats_handler =
                        [metrics = metrics_](const std::uint64_t bytes_in,
                                             const std::uint64_t bytes_out) {
                            metrics->bytes_in.fetch_add(bytes_in, std::memory_order_relaxed);
                            metrics->bytes_out.fetch_add(bytes_out, std::memory_order_relaxed);
                        },
                },
                [weak = weak_from_this()](const std::string_view tunnel_id) {
                    if (auto self = weak.lock()) {
                        return self->resolve_local_endpoint(tunnel_id);
                    }
                    return common::Result<common::Endpoint>::failure(
                        common::ErrorCode::connection_failed,
                        "server session ended before local tunnel lookup");
                });
            if (!workers) {
                co_return disconnected(workers.error().code(), workers.error().message());
            }
            worker_pool_ = std::move(*workers);
            auto workers_started = worker_pool_->start();
            if (!workers_started) {
                co_return disconnected(workers_started.error().code(),
                                       workers_started.error().message());
            }
            backoff_.reset();
            const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - attempt_started);
            static_cast<void>(co_await persist_state(storage::ServerActualState::online, {}, 0U,
                                                     latency.count(), remote_server_id_));
            common::log_info("remote server session is online", log_context());

            const auto heartbeat_interval =
                std::chrono::milliseconds{auth_ok->heartbeat_interval_milliseconds};
            const auto heartbeat_timeout =
                std::clamp(heartbeat_interval * 3, std::chrono::milliseconds{3'000},
                           std::chrono::milliseconds{180'000});
            // Reconcile once immediately after authentication. Later control-plane
            // mutations wake the idle loop directly; heartbeats remain a fallback.
            reconcile_requested_ = true;
            co_return co_await heartbeat_loop(state, heartbeat_timeout);
        }

        enum class IdleWaitResult : std::uint8_t {
            input,
            changed,
            timed_out,
            stopped,
        };

        [[nodiscard]] asio::awaitable<AttemptResult>
        heartbeat_loop(protocol::StateMachine& state,
                       const std::chrono::milliseconds heartbeat_timeout) {
            for (;;) {
                if (reconcile_requested_) {
                    auto reconciled = co_await reconcile_until_current(state, heartbeat_timeout);
                    if (!reconciled) {
                        co_return disconnected(reconciled.error().code(),
                                               reconciled.error().message());
                    }
                }

                const IdleWaitResult ready = co_await wait_for_input_or_change(heartbeat_timeout);
                if (ready == IdleWaitResult::changed) {
                    continue;
                }
                if (ready == IdleWaitResult::timed_out) {
                    co_return disconnected(common::ErrorCode::connection_timeout,
                                           "remote server heartbeat timed out");
                }
                if (ready == IdleWaitResult::stopped) {
                    co_return AttemptResult{AttemptKind::stopped,
                                            common::Error{common::ErrorCode::connection_failed,
                                                          "remote server session stopped"}};
                }

                auto frame = co_await read_frame(state, heartbeat_timeout);
                if (!frame) {
                    co_return disconnected(frame.error().code(), frame.error().message());
                }
                if (frame->type == protocol::MessageType::ping) {
                    auto ping = protocol::decode_heartbeat(frame->payload);
                    if (!ping) {
                        co_return disconnected(ping.error().code(), ping.error().message());
                    }
                    auto timeout_updated = apply_worker_idle_timeout(*ping);
                    if (!timeout_updated) {
                        co_return disconnected(timeout_updated.error().code(),
                                               timeout_updated.error().message());
                    }
                    auto payload = protocol::encode_heartbeat(*ping);
                    if (!payload) {
                        co_return disconnected(payload.error().code(), payload.error().message());
                    }
                    const protocol::Frame pong_frame{protocol::MessageType::pong, 0U,
                                                     frame->request_id, std::move(*payload)};
                    auto written = co_await write_frame(state, pong_frame, heartbeat_timeout);
                    if (!written) {
                        co_return disconnected(written.error().code(), written.error().message());
                    }
                    // Heartbeat liveness must never depend on the number of configured
                    // tunnels. Reconciliation may require one round trip per tunnel, so
                    // acknowledge the server's absolute heartbeat deadline first.
                    reconcile_requested_ = true;
                    auto reconciled = co_await reconcile_until_current(state, heartbeat_timeout);
                    if (!reconciled) {
                        co_return disconnected(reconciled.error().code(),
                                               reconciled.error().message());
                    }
                    continue;
                }
                if (frame->type == protocol::MessageType::request_workers) {
                    auto requested = handle_worker_request(*frame);
                    if (!requested) {
                        co_return disconnected(requested.error().code(),
                                               requested.error().message());
                    }
                    continue;
                }
                if (frame->type == protocol::MessageType::goaway ||
                    frame->type == protocol::MessageType::error) {
                    co_return disconnected(common::ErrorCode::connection_failed,
                                           "remote server closed the control session");
                }
                co_return disconnected(common::ErrorCode::protocol_error,
                                       "remote server sent an unexpected control message");
            }
        }

        [[nodiscard]] asio::awaitable<IdleWaitResult>
        wait_for_input_or_change(const std::chrono::milliseconds timeout) {
            if (stream_ == nullptr || stopping_) {
                co_return IdleWaitResult::stopped;
            }
            if (reconcile_requested_) {
                co_return IdleWaitResult::changed;
            }
            if (SSL_pending(stream_->native_handle()) > 0 ||
                SSL_has_pending(stream_->native_handle()) == 1) {
                co_return IdleWaitResult::input;
            }

            struct WaitState final {
                bool active{true};
                bool readable{false};
                bool failed{false};
            };
            auto state = std::make_shared<WaitState>();
            auto self = shared_from_this();
            idle_wait_active_ = true;
            stream_->lowest_layer().async_wait(
                asio::ip::tcp::socket::wait_read, [self, state](const asio::error_code& error) {
                    if (!state->active) {
                        return;
                    }
                    state->active = false;
                    state->readable = !error;
                    state->failed = static_cast<bool>(error);
                    try {
                        static_cast<void>(self->idle_timer_.cancel());
                    } catch (...) {
                    }
                });

            idle_timer_.expires_after(timeout);
            asio::error_code timer_error;
            co_await idle_timer_.async_wait(asio::redirect_error(asio::use_awaitable, timer_error));
            idle_wait_active_ = false;
            state->active = false;
            if (state->readable) {
                co_return IdleWaitResult::input;
            }

            // Only a readiness wait is outstanding here, so cancellation cannot
            // discard a partially decoded protocol frame.
            asio::error_code ignored;
            stream_->lowest_layer().cancel(ignored);
            if (stopping_ || state->failed) {
                co_return IdleWaitResult::stopped;
            }
            if (reconcile_requested_) {
                co_return IdleWaitResult::changed;
            }
            co_return timer_error ? IdleWaitResult::stopped : IdleWaitResult::timed_out;
        }

        [[nodiscard]] asio::awaitable<common::Result<void>>
        reconcile_until_current(protocol::StateMachine& state,
                                const std::chrono::milliseconds timeout) {
            do {
                reconcile_requested_ = false;
                auto reconciled = co_await reconcile_tunnels(state, timeout);
                if (!reconciled) {
                    co_return reconciled;
                }
            } while (reconcile_requested_ && !stopping_);
            co_return common::Result<void>::success();
        }

        [[nodiscard]] asio::awaitable<common::Result<void>>
        reconcile_tunnels(protocol::StateMachine& state, const std::chrono::milliseconds timeout) {
            auto tunnels = co_await run_db(
                [this] { return repository_.tunnels().list_by_server(server_.id); });
            if (!tunnels) {
                co_return common::Result<void>::failure(tunnels.error());
            }

            std::unordered_map<std::string, std::uint64_t> retained;
            std::unordered_map<std::string, common::Endpoint> desired_local_endpoints;
            for (const auto& tunnel : *tunnels) {
                if (tunnel.desired_state == storage::TunnelDesiredState::active) {
                    const std::string tunnel_id = tunnel.id.str();
                    retained.emplace(tunnel_id, tunnel.config_revision);
                    desired_local_endpoints.emplace(tunnel_id, tunnel.local_endpoint);
                }
            }

            // Release stale listeners before registering replacements. This ordering
            // lets a remove followed immediately by an add reuse the same public port
            // during a single reconciliation cycle. Requests are sent in bounded windows
            // so a large change set does not pay one network round trip per tunnel.
            struct StaleTunnel final {
                std::string id;
                std::uint64_t revision{0U};
            };
            std::vector<StaleTunnel> stale_tunnels;
            stale_tunnels.reserve(registered_tunnels_.size());
            for (const auto& [tunnel_id, registered] : registered_tunnels_) {
                const auto desired = retained.find(tunnel_id);
                if (desired == retained.end() ||
                    desired->second != registered.config_revision) {
                    stale_tunnels.push_back({tunnel_id, registered.config_revision});
                }
            }
            for (std::size_t offset = 0U; offset < stale_tunnels.size();
                 offset += kTunnelSyncWindow) {
                const auto end = std::min(offset + kTunnelSyncWindow, stale_tunnels.size());
                std::vector<std::pair<std::uint64_t, std::string>> requests;
                std::vector<protocol::Frame> frames;
                requests.reserve(end - offset);
                frames.reserve(end - offset);
                for (std::size_t index = offset; index < end; ++index) {
                    const auto& stale = stale_tunnels[index];
                    auto payload = protocol::encode_unregister_tunnel(
                        {stale.id, stale.revision});
                    if (!payload) {
                        co_return common::Result<void>::failure(payload.error());
                    }
                    const std::uint64_t request_id = next_request_id();
                    frames.push_back({protocol::MessageType::unregister_tunnel, 0U, request_id,
                                      std::move(*payload)});
                    requests.emplace_back(request_id, stale.id);
                }
                auto written = co_await write_frames(state, std::move(frames), timeout);
                if (!written) {
                    co_return written;
                }
                for (const auto& [request_id, tunnel_id] : requests) {
                    auto response = co_await read_reconcile_response(state, request_id, timeout);
                    if (!response ||
                        response->type != protocol::MessageType::unregister_tunnel_ok) {
                        co_return common::Result<void>::failure(
                            response ? common::Error{common::ErrorCode::protocol_error,
                                                     "remote tunnel removal response is invalid"}
                                     : response.error());
                    }
                    auto removed = protocol::decode_unregister_tunnel_ok(response->payload);
                    const auto registered = registered_tunnels_.find(tunnel_id);
                    if (!removed || removed->tunnel_id != tunnel_id ||
                        registered == registered_tunnels_.end() ||
                        removed->desired_revision != registered->second.config_revision) {
                        co_return common::Result<void>::failure(
                            common::ErrorCode::protocol_error,
                            "remote tunnel removal response is invalid");
                    }
                    registered_tunnels_.erase(tunnel_id);
                    local_endpoints_.erase(tunnel_id);
                    auto parsed = common::Id::parse(tunnel_id, common::IdKind::tunnel);
                    if (parsed) {
                        const common::Id parsed_id = *parsed;
                        auto current = co_await load_tunnel(parsed_id);
                        if (current) {
                            if (current->desired_state == storage::TunnelDesiredState::removed) {
                                auto erased = co_await erase_tunnel(parsed_id);
                                if (!erased &&
                                    erased.error().code() != common::ErrorCode::not_found) {
                                    co_return erased;
                                }
                            } else {
                                auto updated = co_await persist_tunnel_state(
                                    *parsed, current->config_revision,
                                    storage::TunnelActualState::disabled, {},
                                    true);
                                if (!updated) {
                                    co_return updated;
                                }
                            }
                        }
                    }
                }
            }

            // Purge tombstones left by an interrupted removal or an older build.
            for (const auto& tunnel : *tunnels) {
                if (tunnel.desired_state != storage::TunnelDesiredState::removed) {
                    continue;
                }
                auto erased = co_await erase_tunnel(tunnel.id);
                if (!erased && erased.error().code() != common::ErrorCode::not_found) {
                    co_return erased;
                }
            }

            std::vector<storage::TunnelRecord> pending_registrations;
            pending_registrations.reserve(tunnels->size());
            for (const auto& tunnel : *tunnels) {
                const std::string tunnel_id = tunnel.id.str();
                if (tunnel.desired_state != storage::TunnelDesiredState::active) {
                    continue;
                }
                if (registered_tunnels_.contains(tunnel_id)) {
                    local_endpoints_.insert_or_assign(tunnel_id, tunnel.local_endpoint);
                    if (tunnel.actual_state != storage::TunnelActualState::active) {
                        auto updated = co_await persist_tunnel_state(
                            tunnel.id, tunnel.config_revision,
                            storage::TunnelActualState::active, {}, true);
                        if (!updated) {
                            co_return updated;
                        }
                    }
                    continue;
                }
                pending_registrations.push_back(tunnel);
            }
            for (std::size_t offset = 0U; offset < pending_registrations.size();
                 offset += kTunnelSyncWindow) {
                const auto end = std::min(offset + kTunnelSyncWindow, pending_registrations.size());
                std::vector<std::pair<std::uint64_t, storage::TunnelRecord>> requests;
                std::vector<protocol::Frame> frames;
                requests.reserve(end - offset);
                frames.reserve(end - offset);
                for (std::size_t index = offset; index < end; ++index) {
                    const auto& tunnel = pending_registrations[index];
                    const std::string tunnel_id = tunnel.id.str();
                    auto updating = co_await persist_tunnel_state(
                        tunnel.id, tunnel.config_revision,
                        storage::TunnelActualState::registering, {});
                    if (!updating) {
                        co_return updating;
                    }
                    common::trigger_failpoint("daemon.after_registering_state_commit");
                    auto payload = protocol::encode_register_tunnel({
                        .tunnel_id = tunnel_id,
                        .bind_host = tunnel.remote_endpoint.host(),
                        .bind_port = tunnel.remote_endpoint.port(),
                        .desired_revision = tunnel.config_revision,
                    });
                    if (!payload) {
                        co_return common::Result<void>::failure(payload.error());
                    }
                    const std::uint64_t request_id = next_request_id();
                    frames.push_back({protocol::MessageType::register_tunnel, 0U, request_id,
                                      std::move(*payload)});
                    requests.emplace_back(request_id, tunnel);
                }
                auto written = co_await write_frames(state, std::move(frames), timeout);
                if (!written) {
                    co_return written;
                }
                common::trigger_failpoint("daemon.after_register_request_write");
                for (const auto& [request_id, tunnel] : requests) {
                    const std::string tunnel_id = tunnel.id.str();
                    auto response = co_await read_reconcile_response(state, request_id, timeout);
                    if (!response) {
                        co_return common::Result<void>::failure(response.error());
                    }
                    common::trigger_failpoint("daemon.after_register_response_receive");
                    if (response->type == protocol::MessageType::register_tunnel_ok) {
                        auto accepted = protocol::decode_register_tunnel_ok(response->payload);
                        if (!accepted || accepted->tunnel_id != tunnel_id ||
                            accepted->desired_revision != tunnel.config_revision) {
                            co_return common::Result<void>::failure(
                                common::ErrorCode::protocol_error,
                                "remote tunnel registration response is invalid");
                        }
                        registered_tunnels_.insert_or_assign(
                            tunnel_id,
                            RegisteredTunnel{tunnel.config_revision, tunnel.remote_endpoint});
                        local_endpoints_.insert_or_assign(tunnel_id, tunnel.local_endpoint);
                        auto updated = co_await persist_tunnel_state(
                            tunnel.id, tunnel.config_revision,
                            storage::TunnelActualState::active, {}, true);
                        if (!updated) {
                            co_return updated;
                        }
                        continue;
                    }
                    if (response->type == protocol::MessageType::register_tunnel_error) {
                        auto rejected = protocol::decode_register_tunnel_error(response->payload);
                        if (!rejected || rejected->tunnel_id != tunnel_id ||
                            rejected->desired_revision != tunnel.config_revision) {
                            co_return common::Result<void>::failure(
                                common::ErrorCode::protocol_error,
                                "remote tunnel registration error is invalid");
                        }
                        auto updated = co_await persist_tunnel_state(
                            tunnel.id, tunnel.config_revision,
                            storage::TunnelActualState::failed,
                            PersistenceErrorView{rejected->code,
                                                 "remote tunnel registration was rejected"},
                            true);
                        if (!updated) {
                            co_return updated;
                        }
                        continue;
                    }
                    local_endpoints_.erase(tunnel_id);
                    co_return common::Result<void>::failure(
                        common::ErrorCode::protocol_error,
                        "remote server returned an unexpected tunnel registration response");
                }
            }
            for (auto iterator = local_endpoints_.begin(); iterator != local_endpoints_.end();) {
                if (!desired_local_endpoints.contains(iterator->first)) {
                    iterator = local_endpoints_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            co_return common::Result<void>::success();
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>>
        read_reconcile_response(protocol::StateMachine& state, const std::uint64_t request_id,
                                const std::chrono::milliseconds timeout) {
            const auto buffered = pending_reconcile_responses_.find(request_id);
            if (buffered != pending_reconcile_responses_.end()) {
                protocol::Frame frame = std::move(buffered->second);
                pending_reconcile_responses_.erase(buffered);
                co_return frame;
            }
            for (;;) {
                auto frame = co_await read_frame(state, timeout);
                if (!frame) {
                    co_return frame;
                }
                if (frame->type == protocol::MessageType::ping) {
                    auto ping = protocol::decode_heartbeat(frame->payload);
                    if (!ping) {
                        co_return common::Result<protocol::Frame>::failure(ping.error());
                    }
                    auto timeout_updated = apply_worker_idle_timeout(*ping);
                    if (!timeout_updated) {
                        co_return common::Result<protocol::Frame>::failure(timeout_updated.error());
                    }
                    auto payload = protocol::encode_heartbeat(*ping);
                    if (!payload) {
                        co_return common::Result<protocol::Frame>::failure(payload.error());
                    }
                    const protocol::Frame pong_frame{protocol::MessageType::pong, 0U,
                                                     frame->request_id, std::move(*payload)};
                    auto written = co_await write_frame(state, pong_frame, timeout);
                    if (!written) {
                        co_return common::Result<protocol::Frame>::failure(written.error());
                    }
                    continue;
                }
                if (frame->type == protocol::MessageType::request_workers) {
                    auto requested = handle_worker_request(*frame);
                    if (!requested) {
                        co_return common::Result<protocol::Frame>::failure(requested.error());
                    }
                    continue;
                }
                if (frame->request_id != request_id) {
                    const bool correlated_response =
                        frame->type == protocol::MessageType::register_tunnel_ok ||
                        frame->type == protocol::MessageType::register_tunnel_error ||
                        frame->type == protocol::MessageType::unregister_tunnel_ok;
                    if (!correlated_response || frame->request_id == 0U ||
                        pending_reconcile_responses_.size() >= kTunnelSyncWindow ||
                        pending_reconcile_responses_.contains(frame->request_id)) {
                        co_return common::Result<protocol::Frame>::failure(
                            common::ErrorCode::protocol_error,
                            "remote control response correlation is invalid");
                    }
                    pending_reconcile_responses_.emplace(frame->request_id, std::move(*frame));
                    continue;
                }
                co_return frame;
            }
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>>
        read_frame(protocol::StateMachine& state, const std::chrono::milliseconds timeout) {
            if (stream_ == nullptr) {
                co_return common::Result<protocol::Frame>::failure(
                    common::ErrorCode::connection_failed, "remote stream is unavailable");
            }
            arm_operation_timeout(timeout);
            auto frame = co_await protocol::async_read_frame(*stream_);
            cancel_operation_timeout();
            if (!frame) {
                co_return frame;
            }
            auto transition = state.on_receive(frame->type);
            if (!transition) {
                co_return common::Result<protocol::Frame>::failure(transition.error());
            }
            co_return frame;
        }

        [[nodiscard]] asio::awaitable<common::Result<void>>
        write_frame(protocol::StateMachine& state, const protocol::Frame& frame,
                    const std::chrono::milliseconds timeout) {
            if (stream_ == nullptr) {
                co_return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                        "remote stream is unavailable");
            }
            auto transition = state.on_send(frame.type);
            if (!transition) {
                co_return transition;
            }
            arm_operation_timeout(timeout);
            write_in_progress_ = true;
            auto written = co_await protocol::async_write_frame(*stream_, frame);
            write_in_progress_ = false;
            cancel_operation_timeout();
            co_return written;
        }

        [[nodiscard]] asio::awaitable<common::Result<void>>
        write_frames(protocol::StateMachine& state, std::vector<protocol::Frame> frames,
                     const std::chrono::milliseconds timeout) {
            if (stream_ == nullptr) {
                co_return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                        "remote stream is unavailable");
            }
            for (const auto& frame : frames) {
                auto transition = state.on_send(frame.type);
                if (!transition) {
                    co_return transition;
                }
            }
            arm_operation_timeout(timeout);
            write_in_progress_ = true;
            auto written = co_await protocol::async_write_frames(*stream_, std::move(frames));
            write_in_progress_ = false;
            cancel_operation_timeout();
            co_return written;
        }

        void begin_stop() {
            if (stopping_) {
                return;
            }
            stopping_ = true;
            terminal_state_.store(TerminalState::stopped);
            try {
                static_cast<void>(reconnect_timer_.cancel());
            } catch (...) {
            }
            stop_worker_pool();
            if (stream_ != nullptr && session_generation_ != 0U && !write_in_progress_) {
                goaway_in_progress_ = true;
                auto self = shared_from_this();
                asio::co_spawn(strand_,
                               protocol::async_write_frame(
                                   *stream_, {protocol::MessageType::goaway, 0U, 0U, {}}),
                               [self](const std::exception_ptr&, const common::Result<void>&) {
                                   self->goaway_in_progress_ = false;
                                   self->resolver_.cancel();
                                   self->cancel_timers();
                                   self->close_transport();
                               });
                return;
            }
            resolver_.cancel();
            cancel_timers();
            close_transport();
        }

        template <typename Rep, typename Period>
        void arm_operation_timeout(const std::chrono::duration<Rep, Period> timeout) {
            operation_timer_.expires_after(timeout);
            auto weak = weak_from_this();
            operation_timer_.async_wait([weak](const asio::error_code& error) {
                if (!error) {
                    if (auto self = weak.lock()) {
                        self->resolver_.cancel();
                        self->close_transport();
                    }
                }
            });
        }

        void cancel_operation_timeout() noexcept {
            try {
                static_cast<void>(operation_timer_.cancel());
            } catch (...) {
            }
        }

        void cancel_timers() noexcept {
            try {
                static_cast<void>(reconnect_timer_.cancel());
                static_cast<void>(idle_timer_.cancel());
                static_cast<void>(operation_timer_.cancel());
            } catch (...) {
            }
        }

        void close_transport() noexcept {
            if (stream_ != nullptr) {
                protocol::close_tls_stream(*stream_);
            }
        }

        void stop_worker_pool() noexcept {
            if (worker_pool_ != nullptr) {
                worker_pool_->stop();
                worker_pool_.reset();
            }
        }

        [[nodiscard]] common::Result<void> handle_worker_request(const protocol::Frame& frame) {
            auto requested = protocol::decode_request_workers(frame.payload);
            if (!requested) {
                return common::Result<void>::failure(requested.error());
            }
            if (worker_pool_ == nullptr) {
                return common::Result<void>::failure(
                    common::ErrorCode::protocol_error,
                    "worker request arrived before authentication");
            }
            worker_pool_->request_workers(requested->count);
            return common::Result<void>::success();
        }

        [[nodiscard]] common::Result<void>
        apply_worker_idle_timeout(const protocol::HeartbeatMessage& heartbeat) {
            const auto negotiated =
                protocol::decode_worker_idle_timeout_seconds(heartbeat.sequence);
            if (!negotiated.has_value() || worker_pool_ == nullptr) {
                return common::Result<void>::success();
            }
            return worker_pool_->set_idle_timeout(
                std::chrono::seconds{*negotiated + protocol::kWorkerIdleTimeoutGraceSeconds});
        }

        [[nodiscard]] common::Result<common::Endpoint>
        resolve_local_endpoint(const std::string_view tunnel_id) const {
            const auto endpoint = local_endpoints_.find(std::string{tunnel_id});
            if (endpoint == local_endpoints_.end()) {
                return common::Result<common::Endpoint>::failure(
                    common::ErrorCode::not_found, "Worker referenced an inactive local tunnel");
            }
            return endpoint->second;
        }

        [[nodiscard]] std::uint64_t next_request_id() noexcept {
            const std::uint64_t value = next_request_id_;
            ++next_request_id_;
            if (next_request_id_ == 0U) {
                next_request_id_ = 3U;
            }
            return value;
        }

        [[nodiscard]] asio::awaitable<common::Result<storage::TunnelRecord>>
        load_tunnel(const common::Id& tunnel_id) {
            auto operation = [this, tunnel_id] {
                return repository_.tunnels().get_by_id(tunnel_id);
            };
            return run_db(std::move(operation));
        }

        [[nodiscard]] asio::awaitable<common::Result<void>>
        erase_tunnel(const common::Id& tunnel_id) {
            auto operation = [this, tunnel_id] {
                return repository_.tunnels().erase(tunnel_id);
            };
            return run_db(std::move(operation));
        }

        [[nodiscard]] asio::awaitable<common::Result<void>>
        persist_tunnel_state(const common::Id& tunnel_id,
                             const std::uint64_t expected_revision,
                             const storage::TunnelActualState state,
                             const PersistenceErrorView error = {},
                             const bool synchronized = false) {
            auto operation = [this, tunnel_id, expected_revision, state,
                              error_code = error.code,
                              error_message = std::string{error.message}, synchronized,
                              attempt_generation = attempt_generation_] {
                try {
                    std::optional<common::Error> transition_error;
                    if (error_code.has_value()) {
                        transition_error.emplace(*error_code, error_message);
                    }
                    auto updated = tunnel_reconciler_->transition(
                        server_.id, tunnel_id, attempt_generation, expected_revision,
                        state, transition_error, synchronized);
                    if (!updated) {
                        metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                        common::log_error("failed to persist tunnel state",
                                          log_context(updated.error().code()));
                        return common::Result<void>::failure(updated.error());
                    }
                    return common::Result<void>::success();
                } catch (...) {
                    metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                    common::log_error("exception while persisting tunnel state",
                                      log_context(common::ErrorCode::internal_error));
                    return common::Result<void>::failure(common::ErrorCode::internal_error,
                                                         "failed to persist tunnel state");
                }
            };
            return run_db(std::move(operation));
        }

        [[nodiscard]] asio::awaitable<common::Result<void>>
        mark_tunnels_pending(const PersistenceErrorView error = {}) {
            registered_tunnels_.clear();
            local_endpoints_.clear();
            auto operation = [this, error_code = error.code,
                              error_message = std::string{error.message},
                              attempt_generation = attempt_generation_] {
                try {
                    std::optional<common::Error> persistence_error;
                    if (error_code.has_value()) {
                        persistence_error.emplace(*error_code, error_message);
                    }
                    auto updated = tunnel_reconciler_->end_generation(
                        server_.id, attempt_generation, persistence_error);
                    if (!updated) {
                        metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                        common::log_error(
                            "failed to persist pending tunnel states after disconnect",
                            log_context(updated.error().code()));
                    }
                    if (!updated) {
                        return common::Result<void>::failure(updated.error());
                    }
                    return common::Result<void>::success();
                } catch (...) {
                    metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                    common::log_error("failed to persist pending tunnel states after disconnect",
                                      log_context(common::ErrorCode::internal_error));
                    return common::Result<void>::failure(common::ErrorCode::internal_error,
                                                         "failed to persist pending tunnel states");
                }
            };
            return run_db(std::move(operation));
        }

        [[nodiscard]] asio::awaitable<common::Result<void>>
        persist_state(const storage::ServerActualState state,
                      const PersistenceErrorView error,
                      const std::uint32_t reconnect_attempt,
                      const std::optional<std::int64_t> latency_ms = std::nullopt,
                      const std::string_view remote_server_id = {}) {
            auto operation = [this, state, error_code = error.code,
                              error_message = std::string{error.message}, reconnect_attempt,
                              latency_ms,
                              remote_server_id = std::string{remote_server_id}] {
                    try {
                        const std::scoped_lock lock{persistence_mutex_};
                        if (!persistence_allowed_) {
                            return common::Result<void>::success();
                        }
                        auto transaction = repository_.begin_transaction();
                        if (!transaction) {
                            metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                            common::log_error("failed to begin server state persistence",
                                              log_context(transaction.error().code()));
                            return common::Result<void>::failure(transaction.error());
                        }
                        auto current = repository_.servers().get_by_id(server_.id);
                        if (!current) {
                            static_cast<void>(transaction->rollback());
                            metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                            common::log_error("failed to load server state for persistence",
                                              log_context(current.error().code()));
                            return common::Result<void>::failure(current.error());
                        }
                        if (current->desired_state != storage::ServerDesiredState::enabled ||
                            current->config_revision !=
                                config_revision_.load(std::memory_order_acquire)) {
                            static_cast<void>(transaction->rollback());
                            return common::Result<void>::success();
                        }
                        current->actual_state = state;
                        current->reconnect_attempt = reconnect_attempt;
                        current->latency_ms = latency_ms;
                        if (!remote_server_id.empty()) {
                            current->remote_server_id = remote_server_id;
                        }
                        if (error_code.has_value()) {
                            current->last_error_code = *error_code;
                            current->last_error_message = error_message;
                        } else {
                            current->last_error_code.reset();
                            current->last_error_message.reset();
                        }
                        current->updated_at_unix_ms =
                            std::max(current->updated_at_unix_ms, common::unix_milliseconds_now());
                        auto updated = repository_.servers().update(*current, *transaction);
                        if (!updated) {
                            static_cast<void>(transaction->rollback());
                            metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                            common::log_error("failed to persist server state",
                                              log_context(updated.error().code()));
                            return common::Result<void>::failure(updated.error());
                        }
                        auto committed = transaction->commit();
                        if (!committed) {
                            metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                            common::log_error("failed to commit server state persistence",
                                              log_context(committed.error().code()));
                            return common::Result<void>::failure(committed.error());
                        }
                        return common::Result<void>::success();
                    } catch (...) {
                        metrics_->persistence_errors.fetch_add(1U, std::memory_order_relaxed);
                        common::log_error("exception while persisting server state",
                                          log_context(common::ErrorCode::internal_error));
                        return common::Result<void>::failure(common::ErrorCode::internal_error,
                                                             "failed to persist server state");
                    }
                };
            return run_db(std::move(operation));
        }

        [[nodiscard]] AttemptResult disconnected(const common::ErrorCode code,
                                                 std::string message) const {
            if (code == common::ErrorCode::protocol_error) {
                metrics_->protocol_errors.fetch_add(1U, std::memory_order_relaxed);
            }
            return {AttemptKind::disconnected, common::Error{code, std::move(message)}};
        }

        [[nodiscard]] AttemptResult authentication_failed(const common::ErrorCode code,
                                                          std::string message) const {
            return {AttemptKind::authentication_failed, common::Error{code, std::move(message)}};
        }

        [[nodiscard]] common::LogContext
        log_context(const std::optional<common::ErrorCode> error = std::nullopt) const noexcept {
            return {
                .component = "daemon.server-session",
                .server_id = server_.id.str(),
                .remote_endpoint = remote_endpoint_text_,
                .error_code = error,
            };
        }

        storage::StateRepository& repository_;
        storage::CredentialStore& credentials_;
        common::Id client_id_;
        storage::ServerRecord server_;
        std::string remote_endpoint_text_;
        std::atomic<std::uint64_t> config_revision_{0U};
        std::shared_ptr<asio::ssl::context> tls_context_;
        std::shared_ptr<protocol::TlsSessionCache> tls_session_cache_;
        std::shared_ptr<WorkerBudget> worker_budget_;
        std::shared_ptr<WorkerBudget> connection_budget_;
        std::shared_ptr<TunnelReconciler> tunnel_reconciler_;
        std::shared_ptr<RuntimeMetrics> metrics_;
        std::shared_ptr<asio::thread_pool> db_pool_;
        ServerManagerOptions options_;
        asio::strand<asio::io_context::executor_type> strand_;
        asio::ip::tcp::resolver resolver_;
        asio::steady_timer reconnect_timer_;
        asio::steady_timer idle_timer_;
        asio::steady_timer operation_timer_;
        std::unique_ptr<protocol::TlsStream> stream_;
        std::unique_ptr<WorkerPool> worker_pool_;
        ReconnectBackoff backoff_;
        std::string remote_server_id_;
        std::uint64_t session_generation_{0U};
        std::uint64_t attempt_generation_{0U};
        std::uint64_t next_request_id_{3U};
        struct RegisteredTunnel final {
            std::uint64_t config_revision{0U};
            common::Endpoint remote_endpoint;
        };
        std::unordered_map<std::string, RegisteredTunnel> registered_tunnels_;
        std::unordered_map<std::uint64_t, protocol::Frame> pending_reconcile_responses_;
        std::unordered_map<std::string, common::Endpoint> local_endpoints_;
        std::atomic<TerminalState> terminal_state_{TerminalState::running};
        std::mutex persistence_mutex_;
        bool persistence_allowed_{true};
        bool stopping_{false};
        bool reconcile_requested_{false};
        bool idle_wait_active_{false};
        bool write_in_progress_{false};
        bool goaway_in_progress_{false};
    };

  public:
    [[nodiscard]] static common::Result<std::shared_ptr<Impl>>
    create(asio::io_context& io_context, storage::StateRepository& repository,
           storage::CredentialStore& credentials, common::Id client_id,
           ServerManagerOptions options) {
        auto valid = validate_options(options);
        if (!valid) {
            return common::Result<std::shared_ptr<Impl>>::failure(valid.error());
        }
        if (client_id.kind() != common::IdKind::client) {
            return common::Result<std::shared_ptr<Impl>>::failure(
                common::ErrorCode::invalid_argument, "server manager requires a client ID");
        }
        auto validated_tls_context = protocol::make_client_tls_context({
            .ca_certificate_path = options.ca_certificate_path,
            .ca_certificate_pem = {},
            .client_certificate_pem = {},
            .client_private_key_pem = {},
        });
        if (!validated_tls_context) {
            return common::Result<std::shared_ptr<Impl>>::failure(validated_tls_context.error());
        }
        return std::shared_ptr<Impl>{new Impl(io_context, repository, credentials,
                                              std::move(client_id), std::move(options))};
    }

    ~Impl() { stop(); }

    [[nodiscard]] common::Result<void> start() {
        if (running_.exchange(true)) {
            return common::Result<void>::failure(common::ErrorCode::already_exists,
                                                 "server manager is already running");
        }
        reconcile(false);
        return common::Result<void>::success();
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }
        auto self = shared_from_this();
        asio::dispatch(strand_, [self] {
            try {
                static_cast<void>(self->reconcile_timer_.cancel());
            } catch (...) {
            }
            auto sessions = std::move(self->sessions_);
            self->sessions_.clear();
            self->session_count_.store(0U);
            for (auto& [id, session] : sessions) {
                static_cast<void>(id);
                session->stop();
            }
        });
    }

    void notify_changed() {
        auto self = shared_from_this();
        asio::dispatch(strand_, [self] {
            if (self->running_.load()) {
                self->reconcile(true);
            }
        });
    }

    void reload() {
        auto self = shared_from_this();
        asio::dispatch(strand_, [self] {
            if (!self->running_.load()) {
                return;
            }
            // Session TLS contexts are immutable by design. Recreate sessions
            // so a rotated CA or credential is picked up atomically by the next
            // handshake while existing relay workers drain through stop().
            for (auto& [id, session] : self->sessions_) {
                static_cast<void>(id);
                session->supersede();
            }
            self->sessions_.clear();
            self->session_count_.store(0U);
            self->reconcile(true);
        });
    }

    [[nodiscard]] std::size_t session_count() const noexcept { return session_count_.load(); }

    [[nodiscard]] ipc::Json metrics() const {
        const auto sessions = session_count_.load();
        const auto idle_workers = worker_budget_->in_use();
        const auto total_worker_connections = connection_budget_->in_use();
        const auto active_workers =
            total_worker_connections > idle_workers ? total_worker_connections - idle_workers : 0U;
        const auto persistence_errors =
            metrics_->persistence_errors.load(std::memory_order_relaxed);
        const auto protocol_errors = metrics_->protocol_errors.load(std::memory_order_relaxed);
        return ipc::Json{
            {"sessions", ipc::Json{{"active", sessions}}},
            {"workers", ipc::Json{{"idle", idle_workers},
                                  {"active", active_workers},
                                  {"max_idle", worker_budget_->maximum()}}},
            {"connections", ipc::Json{{"active", active_workers},
                                      {"pending", 0U},
                                      {"max", connection_budget_->maximum()}}},
            {"reconnects", metrics_->reconnects.load(std::memory_order_relaxed)},
            {"tls_resumptions", metrics_->tls_resumptions.load(std::memory_order_relaxed)},
            {"quota_rejections", metrics_->quota_rejections.load(std::memory_order_relaxed)},
            {"errors", persistence_errors + protocol_errors},
            {"persistence_errors", persistence_errors},
            {"protocol_errors", protocol_errors},
            {"throughput",
             ipc::Json{{"bytes_in", metrics_->bytes_in.load(std::memory_order_relaxed)},
                       {"bytes_out", metrics_->bytes_out.load(std::memory_order_relaxed)}}},
        };
    }

  private:
    Impl(asio::io_context& io_context, storage::StateRepository& repository,
         storage::CredentialStore& credentials, common::Id client_id, ServerManagerOptions options)
        : io_context_(io_context), repository_(repository), credentials_(credentials),
          client_id_(std::move(client_id)), options_(std::move(options)),
          worker_budget_(std::make_shared<WorkerBudget>(options_.max_total_idle_workers)),
          connection_budget_(std::make_shared<WorkerBudget>(options_.max_total_connections)),
          tunnel_reconciler_(std::make_shared<TunnelReconciler>(repository_)),
          db_pool_(std::make_shared<asio::thread_pool>(1U)),
          metrics_(std::make_shared<RuntimeMetrics>()), strand_(asio::make_strand(io_context)),
          reconcile_timer_(strand_) {}

    [[nodiscard]] common::Result<std::shared_ptr<asio::ssl::context>>
    make_tls_context(const storage::ServerRecord& server) {
        common::SecureString ca_certificate;
        common::SecureString client_certificate;
        common::SecureString client_private_key;
        if (server.ca_credential_ref.has_value()) {
            auto loaded = credentials_.get(*server.ca_credential_ref);
            if (!loaded) {
                return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                    common::ErrorCode::tls_error, "configured server CA is unavailable");
            }
            ca_certificate = std::move(*loaded);
        }
        if (server.client_certificate_ref.has_value()) {
            auto loaded = credentials_.get(*server.client_certificate_ref);
            if (!loaded) {
                return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                    common::ErrorCode::tls_error, "configured client certificate is unavailable");
            }
            client_certificate = std::move(*loaded);
        }
        if (server.client_private_key_ref.has_value()) {
            auto loaded = credentials_.get(*server.client_private_key_ref);
            if (!loaded) {
                return common::Result<std::shared_ptr<asio::ssl::context>>::failure(
                    common::ErrorCode::tls_error, "configured client private key is unavailable");
            }
            client_private_key = std::move(*loaded);
        }
        return protocol::make_client_tls_context({
            .ca_certificate_path =
                ca_certificate.empty() ? std::string_view{options_.ca_certificate_path}
                                       : std::string_view{},
            .ca_certificate_pem = ca_certificate.view(),
            .client_certificate_pem = client_certificate.view(),
            .client_private_key_pem = client_private_key.view(),
        });
    }

    void purge_removed_tunnels(const storage::ServerRecord& server) {
        auto tunnels = repository_.tunnels().list_by_server(server.id);
        if (!tunnels) {
            common::log_error("failed to inspect removed tunnels during reconciliation",
                              {.component = "daemon.server-manager",
                               .server_id = server.id.str(),
                               .error_code = tunnels.error().code()});
            return;
        }
        for (const auto& tunnel : *tunnels) {
            if (tunnel.desired_state != storage::TunnelDesiredState::removed) {
                continue;
            }
            auto erased = repository_.tunnels().erase(tunnel.id);
            if (!erased && erased.error().code() != common::ErrorCode::not_found) {
                common::log_error("failed to purge removed tunnel state",
                                  {.component = "daemon.server-manager",
                                   .server_id = server.id.str(),
                                   .tunnel_id = tunnel.id.str(),
                                   .error_code = erased.error().code()});
            }
        }
    }

    [[nodiscard]] bool purge_removed_server(const storage::ServerRecord& server) {
        auto credentials_removed = cleanup_all_server_credentials(credentials_, server);
        if (!credentials_removed) {
            common::log_error("failed to purge credentials for removed server",
                              {.component = "daemon.server-manager",
                               .server_id = server.id.str(),
                               .error_code = credentials_removed.error().code()});
            return false;
        }
        auto erased = repository_.servers().erase(server.id);
        if (!erased && erased.error().code() != common::ErrorCode::not_found) {
            common::log_error("failed to purge removed server state",
                              {.component = "daemon.server-manager",
                               .server_id = server.id.str(),
                               .error_code = erased.error().code()});
            return false;
        }
        return true;
    }

    void reconcile(const bool notify_sessions) {
        if (!running_.load()) {
            return;
        }
        auto records = repository_.servers().list();
        if (!records) {
            common::log_error(
                "failed to load servers for reconciliation",
                {.component = "daemon.server-manager", .error_code = records.error().code()});
            schedule_reconcile();
            return;
        }

        std::unordered_set<std::string> retained;
        for (const auto& record : *records) {
            const std::string id = record.id.str();
            if (record.desired_state == storage::ServerDesiredState::removed) {
                const auto existing = sessions_.find(id);
                if (existing != sessions_.end()) {
                    existing->second->supersede();
                    sessions_.erase(existing);
                }
                static_cast<void>(purge_removed_server(record));
                continue;
            }

            purge_removed_tunnels(record);
            if (record.desired_state != storage::ServerDesiredState::enabled ||
                !record.credential_ref.has_value()) {
                const auto existing = sessions_.find(id);
                if (existing != sessions_.end()) {
                    existing->second->supersede();
                    sessions_.erase(existing);
                }
                continue;
            }

            retained.insert(id);
            const auto existing = sessions_.find(id);
            const bool configuration_changed =
                existing != sessions_.end() && !existing->second->matches(record);
            const bool restart_authentication =
                existing != sessions_.end() &&
                existing->second->terminal_state() ==
                    ServerSession::TerminalState::authentication_failed &&
                record.actual_state == storage::ServerActualState::disconnected;
            if (existing != sessions_.end() && !configuration_changed) {
                existing->second->refresh_config_revision(record.config_revision);
            }
            if (existing == sessions_.end() || configuration_changed || restart_authentication) {
                if (existing != sessions_.end()) {
                    existing->second->supersede();
                    sessions_.erase(existing);
                }
                if (configuration_changed ||
                    record.actual_state != storage::ServerActualState::not_authenticated) {
                    auto tls_context = make_tls_context(record);
                    if (!tls_context) {
                        common::log_error(
                            "failed to create isolated TLS context for server session",
                            {.component = "daemon.server-manager",
                             .server_id = record.id.str(),
                             .remote_endpoint = record.endpoint.to_string(),
                             .error_code = tls_context.error().code()});
                        continue;
                    }
                    auto session = std::make_shared<ServerSession>(
                        io_context_, repository_, credentials_, client_id_, record,
                        std::move(*tls_context), worker_budget_, connection_budget_,
                        tunnel_reconciler_, metrics_,
                        db_pool_, options_);
                    sessions_.emplace(id, session);
                    session->start();
                }
            } else if (notify_sessions) {
                existing->second->notify_changed();
            }
        }

        for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
            if (!retained.contains(iterator->first)) {
                iterator->second->supersede();
                iterator = sessions_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        session_count_.store(sessions_.size());
        schedule_reconcile();
    }

    void schedule_reconcile() {
        if (!running_.load()) {
            return;
        }
        reconcile_timer_.expires_after(options_.reconcile_interval);
        auto self = shared_from_this();
        reconcile_timer_.async_wait([self](const asio::error_code& error) {
            if (!error && self->running_.load()) {
                self->reconcile(false);
            }
        });
    }

    asio::io_context& io_context_;
    storage::StateRepository& repository_;
    storage::CredentialStore& credentials_;
    common::Id client_id_;
    ServerManagerOptions options_;
    std::shared_ptr<WorkerBudget> worker_budget_;
    std::shared_ptr<WorkerBudget> connection_budget_;
    std::shared_ptr<TunnelReconciler> tunnel_reconciler_;
    std::shared_ptr<asio::thread_pool> db_pool_;
    std::shared_ptr<RuntimeMetrics> metrics_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer reconcile_timer_;
    std::unordered_map<std::string, std::shared_ptr<ServerSession>> sessions_;
    std::atomic<std::size_t> session_count_{0U};
    std::atomic<bool> running_{false};
};

common::Result<std::unique_ptr<ServerManager>>
ServerManager::create(asio::io_context& io_context, storage::StateRepository& repository,
                      storage::CredentialStore& credentials, common::Id client_id,
                      ServerManagerOptions options) {
    auto implementation =
        Impl::create(io_context, repository, credentials, std::move(client_id), std::move(options));
    if (!implementation) {
        return common::Result<std::unique_ptr<ServerManager>>::failure(implementation.error());
    }
    return std::unique_ptr<ServerManager>{new ServerManager{std::move(*implementation)}};
}

ServerManager::ServerManager(std::shared_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

ServerManager::~ServerManager() noexcept { stop(); }

common::Result<void> ServerManager::start() { return implementation_->start(); }

void ServerManager::stop() noexcept { implementation_->stop(); }

void ServerManager::notify_changed() { implementation_->notify_changed(); }

void ServerManager::reload() { implementation_->reload(); }

std::size_t ServerManager::session_count() const noexcept {
    return implementation_->session_count();
}

ipc::Json ServerManager::metrics() const { return implementation_->metrics(); }

} // namespace minitun::daemon
