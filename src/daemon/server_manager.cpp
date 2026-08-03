#include <minitun/daemon/server_manager.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>

#include <openssl/ssl.h>

#include <minitun/common/error.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/daemon/credential_keys.hpp>
#include <minitun/daemon/reconnect_backoff.hpp>
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
                      std::shared_ptr<WorkerBudget> connection_budget, ServerManagerOptions options)
            : repository_(repository), credentials_(credentials), client_id_(std::move(client_id)),
              server_(std::move(server)), remote_endpoint_text_(server_.endpoint.to_string()),
              tls_context_(std::move(tls_context)), worker_budget_(std::move(worker_budget)),
              connection_budget_(std::move(connection_budget)), options_(std::move(options)),
              strand_(asio::make_strand(io_context)), resolver_(strand_), reconnect_timer_(strand_),
              idle_timer_(strand_), operation_timer_(strand_) {}

        ~ServerSession() {
            stop_worker_pool();
            close_transport();
        }

        void start() {
            auto self = shared_from_this();
            asio::co_spawn(strand_, run(), [self](const std::exception_ptr failure) {
                if (failure) {
                    self->terminal_state_.store(TerminalState::stopped);
                    self->persist_state(storage::ServerActualState::error,
                                        common::Error{common::ErrorCode::internal_error,
                                                      "remote session failed unexpectedly"},
                                        self->backoff_.attempt());
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
                if (self->reconnect_wait_active_) {
                    self->retry_requested_ = true;
                    try {
                        static_cast<void>(self->reconnect_timer_.cancel());
                    } catch (...) {
                    }
                }
            });
        }

        void supersede() {
            {
                const std::scoped_lock lock{persistence_mutex_};
                persistence_allowed_ = false;
            }
            stop();
        }

        [[nodiscard]] bool matches(const storage::ServerRecord& server) const {
            return server_.endpoint == server.endpoint &&
                   server_.credential_ref == server.credential_ref;
        }

        [[nodiscard]] TerminalState terminal_state() const noexcept {
            return terminal_state_.load();
        }

      private:
        enum class AttemptKind : std::uint8_t {
            disconnected,
            authentication_failed,
            stopped,
        };

        struct AttemptResult final {
            AttemptKind kind{AttemptKind::disconnected};
            common::Error error{common::ErrorCode::connection_failed, "remote server disconnected"};
        };

        [[nodiscard]] asio::awaitable<void> run() {
            while (!stopping_) {
                terminal_state_.store(TerminalState::running);
                const AttemptResult result = co_await run_attempt();
                stop_worker_pool();
                close_transport();
                session_generation_ = 0U;
                mark_tunnels_pending(stopping_ ? std::nullopt
                                               : std::optional<common::Error>{result.error});
                if (stopping_ || result.kind == AttemptKind::stopped) {
                    terminal_state_.store(TerminalState::stopped);
                    co_return;
                }
                if (result.kind == AttemptKind::authentication_failed) {
                    terminal_state_.store(TerminalState::authentication_failed);
                    persist_state(storage::ServerActualState::not_authenticated, result.error,
                                  backoff_.attempt());
                    co_return;
                }

                const auto delay = backoff_.next_delay();
                persist_state(storage::ServerActualState::backoff, result.error,
                              backoff_.attempt());
                common::log_warn("remote server connection entered backoff",
                                 log_context(result.error.code()));
                reconnect_timer_.expires_after(delay);
                asio::error_code timer_error;
                reconnect_wait_active_ = true;
                co_await reconnect_timer_.async_wait(
                    asio::redirect_error(asio::use_awaitable, timer_error));
                reconnect_wait_active_ = false;
                if (stopping_) {
                    terminal_state_.store(TerminalState::stopped);
                    co_return;
                }
                if (timer_error && retry_requested_) {
                    retry_requested_ = false;
                    continue;
                }
                if (timer_error) {
                    terminal_state_.store(TerminalState::stopped);
                    co_return;
                }
            }
        }

        [[nodiscard]] asio::awaitable<AttemptResult> run_attempt() {
            const auto attempt_started = std::chrono::steady_clock::now();
            protocol::StateMachine state{protocol::PeerRole::client,
                                         protocol::ConnectionKind::control};
            stream_ = std::make_unique<protocol::TlsStream>(strand_, *tls_context_);
            persist_state(storage::ServerActualState::connecting, std::nullopt, backoff_.attempt());

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

            auto configured = protocol::configure_client_tls_stream(
                *stream_, server_.endpoint.host(), options_.insecure_skip_verify);
            if (!configured) {
                co_return disconnected(configured.error().code(), configured.error().message());
            }
            persist_state(storage::ServerActualState::tls_handshake, std::nullopt,
                          backoff_.attempt());
            arm_operation_timeout(options_.handshake_timeout);
            co_await stream_->async_handshake(asio::ssl::stream_base::client,
                                              asio::redirect_error(asio::use_awaitable, error));
            cancel_operation_timeout();
            if (error) {
                co_return disconnected(common::ErrorCode::tls_error,
                                       "TLS peer verification or handshake failed");
            }

            persist_state(storage::ServerActualState::authenticating, std::nullopt,
                          backoff_.attempt());
            auto token = credentials_.get(credential_key(server_));
            if (!token) {
                co_return authentication_failed(common::ErrorCode::not_authenticated,
                                                "server credential is unavailable");
            }

            auto hello_payload = protocol::encode_hello({client_id_.str()});
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

            const std::int64_t timestamp = common::unix_seconds_now();
            auto digest = protocol::compute_authentication_data(token->view(), client_id_.str(),
                                                                timestamp, ack->nonce);
            if (!digest) {
                co_return disconnected(digest.error().code(), digest.error().message());
            }
            auto auth_payload =
                protocol::encode_auth({client_id_.str(), timestamp, ack->nonce, *digest});
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

            session_generation_ = auth_ok->session_generation;
            remote_server_id_ = ack->server_id;
            const std::uint16_t max_idle_workers = static_cast<std::uint16_t>(std::min<std::size_t>(
                auth_ok->max_idle_workers, options_.max_idle_workers_per_server));
            const std::uint16_t min_idle_workers =
                std::min(auth_ok->min_idle_workers, max_idle_workers);
            auto workers = WorkerPool::create(
                strand_, tls_context_, worker_budget_, connection_budget_,
                {
                    .endpoint = server_.endpoint,
                    .server_id = server_.id.str(),
                    .client_id = client_id_.str(),
                    .session_generation = session_generation_,
                    .min_idle_workers = min_idle_workers,
                    .max_idle_workers = max_idle_workers,
                    .connect_timeout = options_.connect_timeout,
                    .handshake_timeout = options_.handshake_timeout,
                    .idle_timeout = std::chrono::seconds{65},
                    .relay_inactivity_timeout = options_.relay_inactivity_timeout,
                    .graceful_shutdown_timeout = options_.graceful_shutdown_timeout,
                    .insecure_skip_verify = options_.insecure_skip_verify,
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
            persist_state(storage::ServerActualState::online, std::nullopt, 0U, latency.count(),
                          remote_server_id_);
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
            if (SSL_pending(stream_->native_handle()) > 0) {
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
            auto tunnels = repository_.tunnels().list_by_server(server_.id);
            if (!tunnels) {
                co_return common::Result<void>::failure(tunnels.error());
            }

            std::unordered_set<std::string> retained;
            for (const auto& tunnel : *tunnels) {
                if (tunnel.desired_state == storage::TunnelDesiredState::active) {
                    retained.insert(tunnel.id.str());
                }
            }

            // Release stale listeners before registering replacements. This ordering
            // lets a remove followed immediately by an add reuse the same public port
            // during a single reconciliation cycle.
            for (auto iterator = registered_tunnels_.begin();
                 iterator != registered_tunnels_.end();) {
                if (retained.contains(*iterator)) {
                    ++iterator;
                    continue;
                }
                const std::string tunnel_id = *iterator;
                auto payload = protocol::encode_unregister_tunnel({tunnel_id});
                if (!payload) {
                    co_return common::Result<void>::failure(payload.error());
                }
                const std::uint64_t request_id = next_request_id();
                const protocol::Frame removal_frame{protocol::MessageType::unregister_tunnel, 0U,
                                                    request_id, std::move(*payload)};
                auto written = co_await write_frame(state, removal_frame, timeout);
                if (!written) {
                    co_return written;
                }
                auto response = co_await read_reconcile_response(state, request_id, timeout);
                if (!response || response->type != protocol::MessageType::unregister_tunnel_ok) {
                    co_return common::Result<void>::failure(
                        response ? common::Error{common::ErrorCode::protocol_error,
                                                 "remote tunnel removal response is invalid"}
                                 : response.error());
                }
                auto removed = protocol::decode_unregister_tunnel_ok(response->payload);
                if (!removed || removed->tunnel_id != tunnel_id) {
                    co_return common::Result<void>::failure(
                        common::ErrorCode::protocol_error,
                        "remote tunnel removal response is invalid");
                }
                iterator = registered_tunnels_.erase(iterator);
                auto parsed = common::Id::parse(tunnel_id, common::IdKind::tunnel);
                if (parsed) {
                    auto current = repository_.tunnels().get_by_id(*parsed);
                    if (current) {
                        if (current->desired_state == storage::TunnelDesiredState::removed) {
                            auto erased = repository_.tunnels().erase(*parsed);
                            if (!erased && erased.error().code() != common::ErrorCode::not_found) {
                                co_return erased;
                            }
                        } else {
                            auto updated = persist_tunnel_state(
                                *parsed, storage::TunnelActualState::disabled, std::nullopt);
                            if (!updated) {
                                co_return updated;
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
                auto erased = repository_.tunnels().erase(tunnel.id);
                if (!erased && erased.error().code() != common::ErrorCode::not_found) {
                    co_return erased;
                }
            }

            for (const auto& tunnel : *tunnels) {
                const std::string tunnel_id = tunnel.id.str();
                if (tunnel.desired_state != storage::TunnelDesiredState::active) {
                    continue;
                }
                if (registered_tunnels_.contains(tunnel_id)) {
                    if (tunnel.actual_state != storage::TunnelActualState::active) {
                        auto updated = persist_tunnel_state(
                            tunnel.id, storage::TunnelActualState::active, std::nullopt);
                        if (!updated) {
                            co_return updated;
                        }
                    }
                    continue;
                }

                auto updating = persist_tunnel_state(
                    tunnel.id, storage::TunnelActualState::registering, std::nullopt);
                if (!updating) {
                    co_return updating;
                }
                auto payload = protocol::encode_register_tunnel({
                    .tunnel_id = tunnel_id,
                    .bind_host = tunnel.remote_endpoint.host(),
                    .bind_port = tunnel.remote_endpoint.port(),
                });
                if (!payload) {
                    co_return common::Result<void>::failure(payload.error());
                }
                const std::uint64_t request_id = next_request_id();
                const protocol::Frame registration_frame{protocol::MessageType::register_tunnel, 0U,
                                                         request_id, std::move(*payload)};
                auto written = co_await write_frame(state, registration_frame, timeout);
                if (!written) {
                    co_return written;
                }
                auto response = co_await read_reconcile_response(state, request_id, timeout);
                if (!response) {
                    co_return common::Result<void>::failure(response.error());
                }
                if (response->type == protocol::MessageType::register_tunnel_ok) {
                    auto accepted = protocol::decode_register_tunnel_ok(response->payload);
                    if (!accepted || accepted->tunnel_id != tunnel_id) {
                        co_return common::Result<void>::failure(
                            common::ErrorCode::protocol_error,
                            "remote tunnel registration response is invalid");
                    }
                    registered_tunnels_.insert(tunnel_id);
                    auto updated = persist_tunnel_state(
                        tunnel.id, storage::TunnelActualState::active, std::nullopt);
                    if (!updated) {
                        co_return updated;
                    }
                    continue;
                }
                if (response->type == protocol::MessageType::register_tunnel_error) {
                    auto rejected = protocol::decode_register_tunnel_error(response->payload);
                    if (!rejected || rejected->tunnel_id != tunnel_id) {
                        co_return common::Result<void>::failure(
                            common::ErrorCode::protocol_error,
                            "remote tunnel registration error is invalid");
                    }
                    auto updated = persist_tunnel_state(
                        tunnel.id, storage::TunnelActualState::failed,
                        common::Error{rejected->code, "remote tunnel registration was rejected"});
                    if (!updated) {
                        co_return updated;
                    }
                    continue;
                }
                co_return common::Result<void>::failure(
                    common::ErrorCode::protocol_error,
                    "remote server returned an unexpected tunnel registration response");
            }
            co_return common::Result<void>::success();
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>>
        read_reconcile_response(protocol::StateMachine& state, const std::uint64_t request_id,
                                const std::chrono::milliseconds timeout) {
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
                    co_return common::Result<protocol::Frame>::failure(
                        common::ErrorCode::protocol_error,
                        "remote control response request ID does not match");
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
                               [self](const std::exception_ptr, common::Result<void>) {
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

        [[nodiscard]] common::Result<common::Endpoint>
        resolve_local_endpoint(const std::string_view tunnel_id) const {
            auto parsed = common::Id::parse(tunnel_id, common::IdKind::tunnel);
            if (!parsed) {
                return common::Result<common::Endpoint>::failure(
                    common::ErrorCode::protocol_error, "Worker referenced an invalid tunnel ID");
            }
            auto tunnel = repository_.tunnels().get_by_id(*parsed);
            if (!tunnel) {
                return common::Result<common::Endpoint>::failure(tunnel.error());
            }
            if (tunnel->server_id != server_.id ||
                tunnel->desired_state != storage::TunnelDesiredState::active) {
                return common::Result<common::Endpoint>::failure(
                    common::ErrorCode::not_found, "Worker referenced an inactive local tunnel");
            }
            return tunnel->local_endpoint;
        }

        [[nodiscard]] std::uint64_t next_request_id() noexcept {
            const std::uint64_t value = next_request_id_;
            ++next_request_id_;
            if (next_request_id_ == 0U) {
                next_request_id_ = 3U;
            }
            return value;
        }

        [[nodiscard]] common::Result<void>
        persist_tunnel_state(const common::Id& tunnel_id, const storage::TunnelActualState state,
                             const std::optional<common::Error>& error) noexcept {
            try {
                const std::scoped_lock lock{persistence_mutex_};
                if (!persistence_allowed_) {
                    return common::Result<void>::success();
                }
                auto current = repository_.tunnels().get_by_id(tunnel_id);
                if (!current) {
                    return common::Result<void>::failure(current.error());
                }
                if (current->server_id != server_.id) {
                    return common::Result<void>::failure(common::ErrorCode::internal_error,
                                                         "tunnel belongs to another server");
                }
                current->actual_state = state;
                if (error.has_value()) {
                    current->last_error_code = error->code();
                    current->last_error_message = error->message();
                } else {
                    current->last_error_code.reset();
                    current->last_error_message.reset();
                }
                current->updated_at_unix_ms =
                    std::max(current->updated_at_unix_ms, common::unix_milliseconds_now());
                return repository_.tunnels().update(*current);
            } catch (...) {
                return common::Result<void>::failure(common::ErrorCode::internal_error,
                                                     "failed to persist tunnel state");
            }
        }

        void mark_tunnels_pending(const std::optional<common::Error>& error) noexcept {
            registered_tunnels_.clear();
            try {
                auto tunnels = repository_.tunnels().list_by_server(server_.id);
                if (!tunnels) {
                    return;
                }
                for (const auto& tunnel : *tunnels) {
                    if (tunnel.desired_state == storage::TunnelDesiredState::active) {
                        static_cast<void>(persist_tunnel_state(
                            tunnel.id, storage::TunnelActualState::pending, error));
                    }
                }
            } catch (...) {
            }
        }

        void
        persist_state(const storage::ServerActualState state,
                      const std::optional<common::Error> error,
                      const std::uint32_t reconnect_attempt,
                      const std::optional<std::int64_t> latency_ms = std::nullopt,
                      const std::optional<std::string> remote_server_id = std::nullopt) noexcept {
            try {
                const std::scoped_lock lock{persistence_mutex_};
                if (!persistence_allowed_) {
                    return;
                }
                auto transaction = repository_.begin_transaction();
                if (!transaction) {
                    return;
                }
                auto current = repository_.servers().get_by_id(server_.id);
                if (!current) {
                    static_cast<void>(transaction->rollback());
                    return;
                }
                if (current->desired_state != storage::ServerDesiredState::enabled) {
                    static_cast<void>(transaction->rollback());
                    return;
                }
                current->actual_state = state;
                current->reconnect_attempt = reconnect_attempt;
                current->latency_ms = latency_ms;
                if (remote_server_id.has_value()) {
                    current->remote_server_id = remote_server_id;
                }
                if (error.has_value()) {
                    current->last_error_code = error->code();
                    current->last_error_message = error->message();
                } else {
                    current->last_error_code.reset();
                    current->last_error_message.reset();
                }
                current->updated_at_unix_ms =
                    std::max(current->updated_at_unix_ms, common::unix_milliseconds_now());
                auto updated = repository_.servers().update(*current, *transaction);
                if (!updated) {
                    static_cast<void>(transaction->rollback());
                    return;
                }
                static_cast<void>(transaction->commit());
            } catch (...) {
            }
        }

        [[nodiscard]] AttemptResult disconnected(const common::ErrorCode code,
                                                 std::string message) const {
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
        std::shared_ptr<asio::ssl::context> tls_context_;
        std::shared_ptr<WorkerBudget> worker_budget_;
        std::shared_ptr<WorkerBudget> connection_budget_;
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
        std::uint64_t next_request_id_{3U};
        std::unordered_set<std::string> registered_tunnels_;
        std::atomic<TerminalState> terminal_state_{TerminalState::running};
        std::mutex persistence_mutex_;
        bool persistence_allowed_{true};
        bool stopping_{false};
        bool reconcile_requested_{false};
        bool retry_requested_{false};
        bool reconnect_wait_active_{false};
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
        auto validated_tls_context =
            protocol::make_client_tls_context({.ca_certificate_path = options.ca_certificate_path});
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

    [[nodiscard]] std::size_t session_count() const noexcept { return session_count_.load(); }

  private:
    Impl(asio::io_context& io_context, storage::StateRepository& repository,
         storage::CredentialStore& credentials, common::Id client_id, ServerManagerOptions options)
        : io_context_(io_context), repository_(repository), credentials_(credentials),
          client_id_(std::move(client_id)), options_(std::move(options)),
          worker_budget_(std::make_shared<WorkerBudget>(options_.max_total_idle_workers)),
          connection_budget_(std::make_shared<WorkerBudget>(options_.max_total_connections)),
          strand_(asio::make_strand(io_context)), reconcile_timer_(strand_) {}

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
        const auto remove_credential = [this, &server](const std::string_view key) {
            auto removed = credentials_.remove(key);
            if (!removed) {
                common::log_error("failed to purge credentials for removed server",
                                  {.component = "daemon.server-manager",
                                   .server_id = server.id.str(),
                                   .error_code = removed.error().code()});
                return false;
            }
            return true;
        };
        if (server.credential_ref.has_value()) {
            if (!remove_credential(*server.credential_ref)) {
                return false;
            }
        }
        for (const auto& key : managed_credential_keys(server.id)) {
            if (server.credential_ref.has_value() && *server.credential_ref == key) {
                continue;
            }
            if (!remove_credential(key)) {
                return false;
            }
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
            if (existing == sessions_.end() || configuration_changed || restart_authentication) {
                if (existing != sessions_.end()) {
                    existing->second->supersede();
                    sessions_.erase(existing);
                }
                if (configuration_changed ||
                    record.actual_state != storage::ServerActualState::not_authenticated) {
                    auto tls_context = protocol::make_client_tls_context(
                        {.ca_certificate_path = options_.ca_certificate_path});
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
                        std::move(*tls_context), worker_budget_, connection_budget_, options_);
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

std::size_t ServerManager::session_count() const noexcept {
    return implementation_->session_count();
}

} // namespace minitun::daemon
