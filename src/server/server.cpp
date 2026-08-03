#include <minitun/server/server.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/port_range.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/protocol/auth.hpp>
#include <minitun/protocol/messages.hpp>
#include <minitun/protocol/relay.hpp>
#include <minitun/protocol/state_machine.hpp>
#include <minitun/protocol/tls.hpp>
#include <minitun/server/accept_recovery.hpp>
#include <minitun/server/connection_quota.hpp>
#include <minitun/server/session_registry.hpp>
#include <minitun/server/tunnel_registry.hpp>
#include <minitun/server/worker_pool.hpp>

namespace minitun::server {
namespace {

inline constexpr std::size_t kMaxTokenFileBytes = 64U * 1024U;
inline constexpr std::size_t kMaxServerConnections = 100'000U;
inline constexpr std::chrono::seconds kMaxConfiguredTimeout{300};
inline constexpr std::chrono::hours kMaximumRelayTimeout{24};

class FileDescriptor final {
  public:
    explicit FileDescriptor(const int value) noexcept : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return value_; }

  private:
    int value_;
};

[[nodiscard]] common::Result<common::SecureString> load_token_file(const std::string& path) {
    if (path.empty() || path.size() > 4'096U || path.find('\0') != std::string::npos) {
        return common::Result<common::SecureString>::failure(common::ErrorCode::invalid_argument,
                                                             "Token file path is invalid");
    }

    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int raw_descriptor = ::open(path.c_str(), flags);
    if (raw_descriptor < 0) {
        return common::Result<common::SecureString>::failure(
            errno == EACCES ? common::ErrorCode::permission_denied
                            : common::ErrorCode::invalid_argument,
            "Token file cannot be opened");
    }
    const FileDescriptor descriptor{raw_descriptor};

    struct stat status{};
    if (::fstat(descriptor.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return common::Result<common::SecureString>::failure(common::ErrorCode::permission_denied,
                                                             "Token file must be a regular file");
    }
    if (status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
        return common::Result<common::SecureString>::failure(
            common::ErrorCode::permission_denied,
            "Token file must be owned by the server user and inaccessible to group and others");
    }
    if (status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > kMaxTokenFileBytes) {
        return common::Result<common::SecureString>::failure(common::ErrorCode::invalid_argument,
                                                             "Token file size is invalid");
    }

    std::vector<char> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const ssize_t count =
            ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            common::secure_erase_memory(bytes.data(), bytes.size());
            return common::Result<common::SecureString>::failure(
                common::ErrorCode::invalid_argument, "Token file could not be read completely");
        }
        offset += static_cast<std::size_t>(count);
    }

    while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) {
        bytes.pop_back();
    }
    if (bytes.empty()) {
        common::secure_erase_memory(bytes.data(), bytes.capacity());
        return common::Result<common::SecureString>::failure(common::ErrorCode::invalid_argument,
                                                             "Token file contains an empty Token");
    }

    common::SecureString token{{bytes.data(), bytes.size()}};
    common::secure_erase_memory(bytes.data(), bytes.capacity());
    return token;
}

[[nodiscard]] common::Result<void> validate_options(const ServerOptions& options) {
    if (options.max_clients == 0U || options.max_clients > kMaxServerConnections) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "max clients is outside 1..100000");
    }
    if (options.max_tunnels_per_client == 0U || options.max_tunnels_per_client > 4'096U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "max tunnels per client is outside 1..4096");
    }
    if (options.max_connections_per_client == 0U ||
        options.max_connections_per_client > kMaxServerConnections ||
        options.max_total_connections == 0U ||
        options.max_total_connections > kMaxServerConnections ||
        options.max_connections_per_client > options.max_total_connections) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server connection limits are invalid");
    }
    if (options.handshake_timeout <= std::chrono::seconds::zero() ||
        options.handshake_timeout > kMaxConfiguredTimeout ||
        options.heartbeat_interval <= std::chrono::seconds::zero() ||
        options.heartbeat_interval > kMaxConfiguredTimeout ||
        options.heartbeat_timeout <= options.heartbeat_interval ||
        options.heartbeat_timeout > kMaxConfiguredTimeout ||
        options.allowed_clock_skew < std::chrono::seconds::zero() ||
        options.allowed_clock_skew > kMaxConfiguredTimeout ||
        options.worker_wait_timeout <= std::chrono::seconds::zero() ||
        options.worker_wait_timeout > kMaxConfiguredTimeout ||
        options.worker_idle_timeout <= std::chrono::seconds::zero() ||
        options.worker_idle_timeout > kMaxConfiguredTimeout ||
        options.relay_inactivity_timeout <= std::chrono::seconds::zero() ||
        options.relay_inactivity_timeout > kMaximumRelayTimeout ||
        options.graceful_shutdown_timeout <= std::chrono::seconds::zero() ||
        options.graceful_shutdown_timeout > kMaxConfiguredTimeout) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server timeout configuration is invalid");
    }
    if (options.min_idle_workers > options.max_idle_workers || options.max_idle_workers > 128U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server worker limits are invalid");
    }
    if (options.max_total_idle_workers == 0U || options.max_total_idle_workers > 4'096U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server total worker limit is invalid");
    }
    return common::Result<void>::success();
}

} // namespace

class Server::Impl final : public std::enable_shared_from_this<Server::Impl> {
  public:
    [[nodiscard]] static common::Result<std::shared_ptr<Impl>> create(asio::io_context& io_context,
                                                                      ServerOptions options) {
        auto valid = validate_options(options);
        if (!valid) {
            return common::Result<std::shared_ptr<Impl>>::failure(valid.error());
        }
        auto endpoint = common::Endpoint::parse(options.listen_endpoint);
        if (!endpoint) {
            return common::Result<std::shared_ptr<Impl>>::failure(endpoint.error());
        }
        auto allowed_ports = common::PortRange::parse(options.allowed_ports);
        if (!allowed_ports) {
            return common::Result<std::shared_ptr<Impl>>::failure(allowed_ports.error());
        }
        asio::error_code address_error;
        auto address = asio::ip::make_address(endpoint->host(), address_error);
        if (address_error) {
            return common::Result<std::shared_ptr<Impl>>::failure(
                common::ErrorCode::invalid_argument,
                "server listen host must be a numeric IPv4 or IPv6 address");
        }
        auto tls_context = protocol::make_server_tls_context({
            .certificate_chain_path = options.tls_certificate_path,
            .private_key_path = options.tls_private_key_path,
        });
        if (!tls_context) {
            return common::Result<std::shared_ptr<Impl>>::failure(tls_context.error());
        }
        auto token = load_token_file(options.token_file_path);
        if (!token) {
            return common::Result<std::shared_ptr<Impl>>::failure(token.error());
        }
        auto server_id = common::Id::generate(common::IdKind::server);
        if (!server_id) {
            return common::Result<std::shared_ptr<Impl>>::failure(server_id.error());
        }

        return std::shared_ptr<Impl>{new Impl(io_context, std::move(options),
                                              asio::ip::tcp::endpoint{address, endpoint->port()},
                                              std::move(*tls_context), std::move(*token),
                                              server_id->str(), std::move(*allowed_ports))};
    }

    ~Impl() noexcept { stop(); }

    [[nodiscard]] common::Result<void> start() {
        if (running_.exchange(true)) {
            return common::Result<void>::failure(common::ErrorCode::already_exists,
                                                 "TLS server is already running");
        }
        shutting_down_ = false;
        accept_retry_policy_.reset();
        reserved_descriptor_.reopen();

        asio::error_code error;
        acceptor_.open(listen_endpoint_.protocol(), error);
        if (!error) {
            acceptor_.set_option(asio::socket_base::reuse_address{true}, error);
        }
        if (!error) {
            acceptor_.bind(listen_endpoint_, error);
        }
        if (!error) {
            acceptor_.listen(asio::socket_base::max_listen_connections, error);
        }
        if (error) {
            running_.store(false);
            asio::error_code ignored;
            acceptor_.close(ignored);
            return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                 "failed to bind the TLS server listener");
        }

        listening_port_.store(acceptor_.local_endpoint(error).port());
        if (error) {
            running_.store(false);
            acceptor_.close(error);
            return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                 "failed to inspect the TLS server listener");
        }
        accept_next();
        return common::Result<void>::success();
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }
        auto self = shared_from_this();
        asio::dispatch(strand_, [self] { self->begin_shutdown(); });
    }

    [[nodiscard]] std::uint16_t listening_port() const noexcept { return listening_port_.load(); }

    [[nodiscard]] const std::string& server_id() const noexcept { return server_id_; }

  private:
    template <typename Function, typename CompletionToken>
    auto async_run_on_control_strand(Function function, CompletionToken&& token) {
        using Return = std::invoke_result_t<Function>;
        // Keep owning operation state outside Asio's coroutine frame. GCC 11
        // may otherwise duplicate moved lambda state while adapting use_awaitable.
        auto operation = std::make_shared<Function>(std::move(function));
        return asio::async_initiate<CompletionToken, void(Return)>(
            [this, operation](auto handler) mutable {
                auto completion_executor = asio::get_associated_executor(handler);
                asio::post(strand_,
                           [operation, handler = std::move(handler),
                            completion_executor = std::move(completion_executor)]() mutable {
                               auto result = std::make_shared<Return>(std::invoke(*operation));
                               asio::post(std::move(completion_executor),
                                          [handler = std::move(handler),
                                           result = std::move(result)]() mutable {
                                              handler(std::move(*result));
                                          });
                           });
            },
            std::forward<CompletionToken>(token));
    }

    [[nodiscard]] asio::awaitable<common::Result<std::uint64_t>>
    open_client_session(std::string client_id) {
        return async_run_on_control_strand(
            [this, client_id]() mutable {
                auto generation = session_registry_.open(client_id);
                if (generation) {
                    worker_pool_.remove_client(client_id);
                    tunnel_registry_.remove_client(client_id);
                }
                return generation;
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<common::Result<void>>
    register_worker(WorkerRegistration registration, WorkerAssignmentHandler assignment_handler,
                    WorkerRemovalHandler removal_handler) {
        return async_run_on_control_strand(
            [this, registration, assignment_handler, removal_handler]() mutable {
                return worker_pool_.add(std::move(registration), std::move(assignment_handler),
                                        std::move(removal_handler));
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<bool> remove_worker(std::string worker_id) {
        return async_run_on_control_strand(
            [this, worker_id] {
                worker_pool_.remove(worker_id);
                return true;
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<std::size_t> idle_worker_count(std::string client_id,
                                                                 const std::uint64_t generation) {
        return async_run_on_control_strand(
            [this, client_id, generation] {
                return worker_pool_.idle_count(client_id, generation);
            },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<common::Result<void>> register_tunnel(TunnelBinding binding) {
        return async_run_on_control_strand(
            [this, binding] { return tunnel_registry_.register_tunnel(binding); },
            asio::use_awaitable);
    }

    [[nodiscard]] asio::awaitable<bool> unregister_tunnel(std::string client_id,
                                                          const std::uint64_t generation,
                                                          std::string tunnel_id) {
        return async_run_on_control_strand(
            [this, client_id, generation, tunnel_id] {
                tunnel_registry_.unregister_tunnel(client_id, generation, tunnel_id);
                return true;
            },
            asio::use_awaitable);
    }

    class ControlSession final : public std::enable_shared_from_this<ControlSession> {
      public:
        ControlSession(asio::ip::tcp::socket socket, std::shared_ptr<Impl> server)
            : server_(std::move(server)), stream_(std::move(socket), *server_->tls_context_),
              operation_timer_(stream_.get_executor()), heartbeat_timer_(stream_.get_executor()) {
            asio::error_code endpoint_error;
            const auto endpoint = stream_.lowest_layer().remote_endpoint(endpoint_error);
            remote_endpoint_ = endpoint_error ? std::string{} : endpoint.address().to_string();
            auto connection_id = common::Id::generate(common::IdKind::connection);
            if (connection_id) {
                connection_id_ = connection_id->str();
            }
        }

        ~ControlSession() { protocol::close_tls_stream(stream_); }

        void start() {
            auto self = shared_from_this();
            asio::co_spawn(stream_.get_executor(), run(), [self](const std::exception_ptr failure) {
                if (failure) {
                    common::log_warn("remote control session ended with an exception",
                                     self->log_context(common::ErrorCode::internal_error));
                }
                self->run_finished_ = true;
                self->cancel_timers();
                if (!self->goaway_in_progress_) {
                    self->close_transport();
                    self->notify_finished();
                }
            });
        }

        void request_stop(const bool graceful) {
            auto self = shared_from_this();
            asio::dispatch(stream_.get_executor(),
                           [self, graceful] { self->request_stop_on_executor(graceful); });
        }

        void force_stop() noexcept {
            auto self = shared_from_this();
            asio::dispatch(stream_.get_executor(), [self] { self->force_stop_on_executor(); });
        }

        [[nodiscard]] bool relay_active() const noexcept { return relay_active_.load(); }

        [[nodiscard]] asio::any_io_executor executor() noexcept { return stream_.get_executor(); }

        void cleanup_on_control_strand() noexcept {
            if (cleanup_complete_) {
                return;
            }
            cleanup_complete_ = true;
            if (worker_registered_) {
                server_->worker_pool_.remove(worker_id_);
                worker_registered_ = false;
            }
            if (control_connection_ && generation_ != 0U && !client_id_.empty()) {
                server_->worker_pool_.remove_session(client_id_, generation_);
                server_->tunnel_registry_.remove_session(client_id_, generation_);
                server_->session_registry_.close(client_id_, generation_);
            }
            server_->active_connections_.fetch_sub(1U);
        }

      private:
        void request_stop_on_executor(const bool graceful) {
            if (stop_requested_) {
                return;
            }
            stop_requested_ = true;
            if (graceful && control_connection_ && generation_ != 0U && state_.has_value() &&
                state_->state() == protocol::ConnectionState::authenticated &&
                !write_in_progress_) {
                send_goaway();
                return;
            }
            force_stop_on_executor();
        }

        void force_stop_on_executor() noexcept {
            stop_requested_ = true;
            cancel_timers();
            close_transport();
        }
        struct WorkerAssignment final {
            TunnelBinding binding;
            asio::ip::tcp::socket public_socket;
            ConnectionQuota::Lease connection_lease;
        };

        [[nodiscard]] asio::awaitable<void> run() {
            const bool handshake_complete = co_await perform_tls_handshake();
            if (!handshake_complete) {
                co_return;
            }
            auto first_frame = co_await read_initial_frame();
            if (!first_frame) {
                co_return;
            }
            if (first_frame->type == protocol::MessageType::worker_hello) {
                state_.emplace(protocol::PeerRole::server, protocol::ConnectionKind::worker);
                if (!state_->on_receive(first_frame->type)) {
                    co_return;
                }
                co_await run_worker(*first_frame);
                co_return;
            }
            if (first_frame->type != protocol::MessageType::hello) {
                co_return;
            }
            control_connection_ = true;
            state_.emplace(protocol::PeerRole::server, protocol::ConnectionKind::control);
            const auto transition = state_->on_receive(first_frame->type);
            if (!transition) {
                co_return;
            }
            const bool authenticated = co_await authenticate(*first_frame);
            if (!authenticated) {
                co_return;
            }
            common::log_info("remote client authenticated", log_context());
            co_await heartbeat_loop();
        }

        [[nodiscard]] asio::awaitable<bool> perform_tls_handshake() {
            arm_operation_timeout(server_->options_.handshake_timeout);
            asio::error_code error;
            co_await stream_.async_handshake(asio::ssl::stream_base::server,
                                             asio::redirect_error(asio::use_awaitable, error));
            cancel_operation_timeout();
            if (error) {
                common::log_warn("TLS handshake failed", log_context(common::ErrorCode::tls_error));
                co_return false;
            }
            co_return true;
        }

        [[nodiscard]] asio::awaitable<bool> authenticate(const protocol::Frame& hello_frame) {
            auto hello = protocol::decode_hello(hello_frame.payload);
            if (!hello) {
                co_return false;
            }
            client_id_ = hello->client_id;

            auto nonce = protocol::generate_authentication_nonce();
            if (!nonce) {
                co_return false;
            }
            challenge_nonce_ = *nonce;
            auto ack_payload = protocol::encode_hello_ack({
                .server_id = server_->server_id_,
                .server_time_seconds = common::unix_seconds_now(),
                .nonce = challenge_nonce_,
            });
            if (!ack_payload) {
                co_return false;
            }
            const protocol::Frame hello_ack_frame{protocol::MessageType::hello_ack, 0U,
                                                  hello_frame.request_id, std::move(*ack_payload)};
            if (!co_await write_frame(hello_ack_frame, server_->options_.handshake_timeout)) {
                co_return false;
            }

            auto auth_frame = co_await read_frame(server_->options_.handshake_timeout);
            if (!auth_frame || auth_frame->type != protocol::MessageType::auth) {
                co_return false;
            }
            auto auth = protocol::decode_auth(auth_frame->payload);
            if (!auth) {
                co_return co_await reject_authentication(auth_frame->request_id);
            }

            const auto now = std::chrono::steady_clock::now();
            bool accepted = server_->auth_rate_limiter_.allowed(remote_endpoint_, now);
            accepted = accepted && auth->client_id == client_id_;
            accepted = accepted && auth->nonce == challenge_nonce_;
            if (accepted) {
                auto skew = common::is_clock_skew_within(auth->timestamp_seconds,
                                                         common::unix_seconds_now(),
                                                         server_->options_.allowed_clock_skew);
                accepted = skew && *skew;
            }
            if (accepted) {
                auto fresh = server_->nonce_cache_.consume(auth->nonce, now);
                accepted = fresh && *fresh;
            }
            if (accepted) {
                auto verified = protocol::verify_authentication_data(
                    server_->token_.view(), auth->client_id, auth->timestamp_seconds, auth->nonce,
                    auth->authentication_data);
                accepted = verified && *verified;
            }
            if (!accepted) {
                server_->auth_rate_limiter_.record_failure(remote_endpoint_, now);
                co_return co_await reject_authentication(auth_frame->request_id);
            }

            auto generation = co_await server_->open_client_session(client_id_);
            if (!generation) {
                auto error_payload =
                    protocol::encode_auth_error({common::ErrorCode::resource_exhausted});
                if (error_payload) {
                    const protocol::Frame auth_error_frame{protocol::MessageType::auth_error, 0U,
                                                           auth_frame->request_id,
                                                           std::move(*error_payload)};
                    static_cast<void>(co_await write_frame(auth_error_frame,
                                                           server_->options_.handshake_timeout));
                }
                co_return false;
            }
            generation_ = *generation;

            const auto heartbeat_milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    server_->options_.heartbeat_interval);
            auto ok_payload = protocol::encode_auth_ok({
                .session_generation = generation_,
                .heartbeat_interval_milliseconds =
                    static_cast<std::uint32_t>(heartbeat_milliseconds.count()),
                .min_idle_workers = server_->options_.min_idle_workers,
                .max_idle_workers = server_->options_.max_idle_workers,
            });
            if (!ok_payload) {
                co_return false;
            }
            const protocol::Frame auth_ok_frame{protocol::MessageType::auth_ok, 0U,
                                                auth_frame->request_id, std::move(*ok_payload)};
            if (!co_await write_frame(auth_ok_frame, server_->options_.handshake_timeout)) {
                co_return false;
            }
            server_->auth_rate_limiter_.record_success(remote_endpoint_);
            co_return true;
        }

        [[nodiscard]] asio::awaitable<bool> reject_authentication(const std::uint64_t request_id) {
            auto payload = protocol::encode_auth_error({common::ErrorCode::authentication_failed});
            if (payload) {
                const protocol::Frame auth_error_frame{protocol::MessageType::auth_error, 0U,
                                                       request_id, std::move(*payload)};
                static_cast<void>(
                    co_await write_frame(auth_error_frame, server_->options_.handshake_timeout));
            }
            common::log_warn("remote client authentication failed",
                             log_context(common::ErrorCode::authentication_failed));
            co_return false;
        }

        [[nodiscard]] asio::awaitable<void> run_worker(const protocol::Frame& hello_frame) {
            auto hello = protocol::decode_worker_hello(hello_frame.payload);
            if (!hello || !server_->session_registry_.is_current(hello->client_id,
                                                                 hello->session_generation)) {
                co_return;
            }
            client_id_ = hello->client_id;
            generation_ = hello->session_generation;
            worker_id_ = hello->worker_id;

            auto accepted_payload = protocol::encode_worker_accepted({worker_id_});
            if (!accepted_payload) {
                co_return;
            }
            const protocol::Frame accepted_frame{protocol::MessageType::worker_accepted, 0U,
                                                 hello_frame.request_id,
                                                 std::move(*accepted_payload)};
            if (!co_await write_frame(accepted_frame, server_->options_.handshake_timeout)) {
                co_return;
            }

            // Named lvalues force independent ownership before this coroutine
            // suspends, including with GCC 11 and Asio 1.18.
            const auto weak = weak_from_this();
            const WorkerRegistration registration{client_id_, generation_, worker_id_};
            const WorkerAssignmentHandler assignment_handler =
                [weak](TunnelBinding binding, asio::ip::tcp::socket public_socket,
                       ConnectionQuota::Lease connection_lease) mutable {
                    if (auto self = weak.lock()) {
                        asio::post(self->stream_.get_executor(),
                                   [self, binding = std::move(binding),
                                    public_socket = std::move(public_socket),
                                    connection_lease = std::move(connection_lease)]() mutable {
                                       if (self->run_finished_ || self->stop_requested_) {
                                           asio::error_code ignored;
                                           public_socket.close(ignored);
                                           return;
                                       }
                                       self->worker_registered_ = false;
                                       self->worker_assignment_ =
                                           std::make_unique<WorkerAssignment>(WorkerAssignment{
                                               std::move(binding), std::move(public_socket),
                                               std::move(connection_lease)});
                                       try {
                                           static_cast<void>(self->heartbeat_timer_.cancel());
                                       } catch (...) {
                                       }
                                   });
                    } else {
                        asio::error_code ignored;
                        public_socket.close(ignored);
                    }
                };
            const WorkerRemovalHandler removal_handler = [weak] {
                if (auto self = weak.lock()) {
                    asio::post(self->stream_.get_executor(), [self] {
                        self->worker_registered_ = false;
                        self->force_stop_on_executor();
                    });
                }
            };
            auto registered = co_await server_->register_worker(registration, assignment_handler,
                                                                removal_handler);
            if (!registered) {
                co_return;
            }
            worker_registered_ = true;

            heartbeat_timer_.expires_after(server_->options_.worker_idle_timeout);
            server_->notify_worker_available(client_id_, generation_);
            asio::error_code idle_error;
            co_await heartbeat_timer_.async_wait(
                asio::redirect_error(asio::use_awaitable, idle_error));
            if (worker_assignment_ == nullptr) {
                static_cast<void>(co_await server_->remove_worker(worker_id_));
                worker_registered_ = false;
                co_return;
            }
            co_await handle_worker_assignment();
        }

        [[nodiscard]] asio::awaitable<void> handle_worker_assignment() {
            if (worker_assignment_ == nullptr) {
                co_return;
            }
            auto connection_id = common::Id::generate(common::IdKind::connection);
            if (!connection_id) {
                co_return;
            }
            const std::string connection_id_text = connection_id->str();
            auto relay_payload = protocol::encode_start_relay(
                {worker_assignment_->binding.tunnel_id, connection_id_text});
            if (!relay_payload) {
                co_return;
            }
            const protocol::Frame relay_frame{protocol::MessageType::start_relay, 0U, 1U,
                                              std::move(*relay_payload)};
            if (!co_await write_frame(relay_frame, server_->options_.handshake_timeout)) {
                co_return;
            }
            auto local_result = co_await read_frame(server_->options_.handshake_timeout);
            if (!local_result) {
                co_return;
            }
            if (local_result->type == protocol::MessageType::local_connect_error) {
                auto failed = protocol::decode_local_connect_error(local_result->payload);
                if (!failed || failed->connection_id != connection_id_text) {
                    co_return;
                }
                co_return;
            }
            if (local_result->type != protocol::MessageType::local_connect_ok) {
                co_return;
            }
            auto connected = protocol::decode_local_connect_ok(local_result->payload);
            if (!connected || connected->connection_id != connection_id_text) {
                co_return;
            }
            relay_active_.store(true);
            auto relayed = co_await protocol::relay_tls_and_tcp(
                stream_, worker_assignment_->public_socket,
                {.inactivity_timeout = server_->options_.relay_inactivity_timeout});
            relay_active_.store(false);
            if (!relayed && relayed.error().code() != common::ErrorCode::connection_timeout) {
                common::log_warn("public relay ended with a transport error",
                                 log_context(relayed.error().code()));
            }
        }

        [[nodiscard]] asio::awaitable<void> heartbeat_loop() {
            std::uint64_t sequence = 1U;
            for (;;) {
                if (!server_->session_registry_.is_current(client_id_, generation_)) {
                    co_return;
                }
                const auto next_heartbeat =
                    std::chrono::steady_clock::now() + server_->options_.heartbeat_interval;
                for (;;) {
                    const IdleWaitResult ready =
                        co_await wait_for_heartbeat_or_input(next_heartbeat);
                    if (ready == IdleWaitResult::stopped) {
                        co_return;
                    }
                    if (ready == IdleWaitResult::heartbeat_due) {
                        break;
                    }
                    auto frame = co_await read_frame(server_->options_.heartbeat_timeout);
                    if (!frame || !co_await handle_control_request(
                                      *frame, server_->options_.heartbeat_timeout)) {
                        co_return;
                    }
                }

                const std::size_t idle_workers =
                    co_await server_->idle_worker_count(client_id_, generation_);
                if (idle_workers < server_->options_.min_idle_workers) {
                    const auto missing = static_cast<std::uint16_t>(
                        server_->options_.min_idle_workers - idle_workers);
                    auto request_payload = protocol::encode_request_workers({missing});
                    if (!request_payload) {
                        co_return;
                    }
                    const protocol::Frame request_frame{protocol::MessageType::request_workers, 0U,
                                                        sequence, std::move(*request_payload)};
                    if (!co_await write_frame(request_frame, server_->options_.heartbeat_timeout)) {
                        co_return;
                    }
                }

                auto ping_payload = protocol::encode_heartbeat({sequence});
                if (!ping_payload) {
                    co_return;
                }
                const protocol::Frame ping_frame{protocol::MessageType::ping, 0U, sequence,
                                                 std::move(*ping_payload)};
                if (!co_await write_frame(ping_frame, server_->options_.heartbeat_timeout)) {
                    co_return;
                }

                const auto deadline =
                    std::chrono::steady_clock::now() + server_->options_.heartbeat_timeout;
                bool received_pong = false;
                while (!received_pong) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        common::log_warn("remote client heartbeat timed out",
                                         log_context(common::ErrorCode::connection_timeout));
                        co_return;
                    }
                    const auto remaining = std::chrono::ceil<std::chrono::seconds>(deadline - now);
                    auto frame = co_await read_frame(remaining);
                    if (!frame) {
                        co_return;
                    }
                    if (!server_->session_registry_.is_current(client_id_, generation_)) {
                        co_return;
                    }
                    if (frame->type == protocol::MessageType::pong) {
                        auto pong = protocol::decode_heartbeat(frame->payload);
                        if (!pong || pong->sequence != sequence || frame->request_id != sequence) {
                            co_return;
                        }
                        received_pong = true;
                        continue;
                    }
                    if (!co_await handle_control_request(*frame, remaining)) {
                        co_return;
                    }
                }
                ++sequence;
                if (sequence == 0U) {
                    sequence = 1U;
                }
            }
        }

        enum class IdleWaitResult : std::uint8_t {
            heartbeat_due,
            readable,
            stopped,
        };

        [[nodiscard]] asio::awaitable<IdleWaitResult>
        wait_for_heartbeat_or_input(const std::chrono::steady_clock::time_point deadline) {
            struct WaitState final {
                bool active{true};
                bool readable{false};
            };

            auto state = std::make_shared<WaitState>();
            auto self = shared_from_this();
            stream_.lowest_layer().async_wait(
                asio::ip::tcp::socket::wait_read, [self, state](const asio::error_code& error) {
                    if (!error && state->active) {
                        state->active = false;
                        state->readable = true;
                        static_cast<void>(self->heartbeat_timer_.cancel());
                    }
                });

            heartbeat_timer_.expires_at(deadline);
            asio::error_code timer_error;
            co_await heartbeat_timer_.async_wait(
                asio::redirect_error(asio::use_awaitable, timer_error));
            state->active = false;
            if (state->readable) {
                co_return IdleWaitResult::readable;
            }

            // Only the readiness wait is outstanding while the control session is
            // idle. Cancel it before starting the next framed operation; socket
            // cancellation does not close the established TCP connection.
            asio::error_code ignored;
            stream_.lowest_layer().cancel(ignored);
            co_return timer_error ? IdleWaitResult::stopped : IdleWaitResult::heartbeat_due;
        }

        [[nodiscard]] asio::awaitable<bool>
        handle_control_request(const protocol::Frame& frame, const std::chrono::seconds timeout) {
            if (frame.type == protocol::MessageType::ping) {
                auto ping = protocol::decode_heartbeat(frame.payload);
                if (!ping) {
                    co_return false;
                }
                auto pong_payload = protocol::encode_heartbeat(*ping);
                if (!pong_payload) {
                    co_return false;
                }
                const protocol::Frame pong_frame{protocol::MessageType::pong, 0U, frame.request_id,
                                                 std::move(*pong_payload)};
                co_return co_await write_frame(pong_frame, timeout);
            }
            if (frame.type == protocol::MessageType::register_tunnel) {
                co_return co_await handle_register_tunnel(frame, timeout);
            }
            if (frame.type == protocol::MessageType::unregister_tunnel) {
                co_return co_await handle_unregister_tunnel(frame, timeout);
            }
            co_return false;
        }

        [[nodiscard]] asio::awaitable<bool>
        handle_register_tunnel(const protocol::Frame& frame, const std::chrono::seconds timeout) {
            auto registration = protocol::decode_register_tunnel(frame.payload);
            if (!registration) {
                co_return false;
            }
            // Keep this aggregate out of the co_await expression so its strings
            // have unambiguous ownership across older coroutine implementations.
            const TunnelBinding binding{
                .client_id = client_id_,
                .session_generation = generation_,
                .tunnel_id = registration->tunnel_id,
                .bind_host = registration->bind_host,
                .bind_port = registration->bind_port,
            };
            auto registered = co_await server_->register_tunnel(binding);
            if (!registered) {
                auto payload = protocol::encode_register_tunnel_error(
                    {registration->tunnel_id, registered.error().code()});
                if (!payload) {
                    co_return false;
                }
                const protocol::Frame error_frame{protocol::MessageType::register_tunnel_error, 0U,
                                                  frame.request_id, std::move(*payload)};
                co_return co_await write_frame(error_frame, timeout);
            }
            auto payload = protocol::encode_register_tunnel_ok({registration->tunnel_id});
            if (!payload) {
                co_return false;
            }
            const protocol::Frame accepted_frame{protocol::MessageType::register_tunnel_ok, 0U,
                                                 frame.request_id, std::move(*payload)};
            co_return co_await write_frame(accepted_frame, timeout);
        }

        [[nodiscard]] asio::awaitable<bool>
        handle_unregister_tunnel(const protocol::Frame& frame, const std::chrono::seconds timeout) {
            auto removal = protocol::decode_unregister_tunnel(frame.payload);
            if (!removal) {
                co_return false;
            }
            static_cast<void>(
                co_await server_->unregister_tunnel(client_id_, generation_, removal->tunnel_id));
            auto payload = protocol::encode_unregister_tunnel_ok({removal->tunnel_id});
            if (!payload) {
                co_return false;
            }
            const protocol::Frame removed_frame{protocol::MessageType::unregister_tunnel_ok, 0U,
                                                frame.request_id, std::move(*payload)};
            co_return co_await write_frame(removed_frame, timeout);
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>> read_initial_frame() {
            arm_operation_timeout(server_->options_.handshake_timeout);
            auto frame = co_await protocol::async_read_frame(stream_);
            cancel_operation_timeout();
            co_return frame;
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>>
        read_frame(const std::chrono::seconds timeout) {
            arm_operation_timeout(timeout);
            auto frame = co_await protocol::async_read_frame(stream_);
            cancel_operation_timeout();
            if (!frame) {
                co_return frame;
            }
            if (!state_.has_value()) {
                co_return common::Result<protocol::Frame>::failure(
                    common::ErrorCode::internal_error, "remote connection state is unavailable");
            }
            auto transition = state_->on_receive(frame->type);
            if (!transition) {
                co_return common::Result<protocol::Frame>::failure(transition.error());
            }
            co_return frame;
        }

        [[nodiscard]] asio::awaitable<bool> write_frame(const protocol::Frame& frame,
                                                        const std::chrono::seconds timeout) {
            if (!state_.has_value()) {
                co_return false;
            }
            auto transition = state_->on_send(frame.type);
            if (!transition) {
                co_return false;
            }
            arm_operation_timeout(timeout);
            write_in_progress_ = true;
            auto written = co_await protocol::async_write_frame(stream_, frame);
            write_in_progress_ = false;
            cancel_operation_timeout();
            co_return static_cast<bool>(written);
        }

        void send_goaway() {
            auto transition = state_->on_send(protocol::MessageType::goaway);
            if (!transition) {
                force_stop();
                return;
            }
            goaway_in_progress_ = true;
            auto self = shared_from_this();
            asio::co_spawn(
                stream_.get_executor(),
                protocol::async_write_frame(stream_, {protocol::MessageType::goaway, 0U, 0U, {}}),
                [self](const std::exception_ptr, common::Result<void>) {
                    self->goaway_in_progress_ = false;
                    self->cancel_timers();
                    self->close_transport();
                    if (self->run_finished_) {
                        self->notify_finished();
                    }
                });
        }

        void close_transport() noexcept {
            if (worker_assignment_ != nullptr) {
                asio::error_code ignored;
                worker_assignment_->public_socket.cancel(ignored);
                worker_assignment_->public_socket.close(ignored);
            }
            protocol::close_tls_stream(stream_);
        }

        void notify_finished() noexcept {
            if (finished_notified_) {
                return;
            }
            finished_notified_ = true;
            server_->session_finished(shared_from_this());
        }

        void arm_operation_timeout(const std::chrono::seconds timeout) {
            operation_timer_.expires_after(timeout);
            auto weak = weak_from_this();
            operation_timer_.async_wait([weak](const asio::error_code& error) {
                if (!error) {
                    if (auto self = weak.lock()) {
                        protocol::close_tls_stream(self->stream_);
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
                static_cast<void>(operation_timer_.cancel());
                static_cast<void>(heartbeat_timer_.cancel());
            } catch (...) {
            }
        }

        [[nodiscard]] common::LogContext
        log_context(const std::optional<common::ErrorCode> error = std::nullopt) const noexcept {
            return {
                .component = "server.control",
                .server_id = server_->server_id_,
                .connection_id = connection_id_,
                .remote_endpoint = remote_endpoint_,
                .error_code = error,
            };
        }

        std::shared_ptr<Impl> server_;
        protocol::TlsStream stream_;
        asio::steady_timer operation_timer_;
        asio::steady_timer heartbeat_timer_;
        std::optional<protocol::StateMachine> state_;
        std::string remote_endpoint_;
        std::string connection_id_;
        std::string client_id_;
        std::string worker_id_;
        protocol::AuthenticationNonce challenge_nonce_{};
        std::uint64_t generation_{0U};
        std::unique_ptr<WorkerAssignment> worker_assignment_;
        bool control_connection_{false};
        bool worker_registered_{false};
        std::atomic<bool> relay_active_{false};
        bool write_in_progress_{false};
        bool stop_requested_{false};
        bool goaway_in_progress_{false};
        bool run_finished_{false};
        bool finished_notified_{false};
        bool cleanup_complete_{false};
    };

    class PendingPublicConnection final
        : public std::enable_shared_from_this<PendingPublicConnection> {
      public:
        PendingPublicConnection(std::shared_ptr<Impl> server, TunnelBinding binding,
                                asio::ip::tcp::socket public_socket,
                                ConnectionQuota::Lease connection_lease)
            : server_(std::move(server)), binding_(std::move(binding)),
              session_key_(pending_session_key(binding_.client_id, binding_.session_generation)),
              public_socket_(std::move(public_socket)), deadline_timer_(server_->strand_),
              connection_lease_(std::move(connection_lease)) {}

        ~PendingPublicConnection() noexcept { close(); }

        void start(const bool try_immediately) {
            if ((try_immediately && try_assign()) || finished_) {
                return;
            }
            deadline_timer_.expires_after(server_->options_.worker_wait_timeout);
            auto self = shared_from_this();
            deadline_timer_.async_wait([self](const asio::error_code& error) {
                if (!error) {
                    self->stop();
                }
            });
        }

        [[nodiscard]] const std::string& session_key() const noexcept { return session_key_; }

        [[nodiscard]] bool try_assign() {
            if (finished_) {
                return false;
            }
            if (!server_->running_.load() || !server_->session_registry_.is_current(
                                                 binding_.client_id, binding_.session_generation)) {
                stop();
                return false;
            }
            if (!server_->worker_pool_.assign(binding_, public_socket_, connection_lease_)) {
                return false;
            }
            finish();
            return true;
        }

        void stop() noexcept {
            close();
            finish();
        }

      private:
        void close() noexcept {
            asio::error_code ignored;
            try {
                static_cast<void>(deadline_timer_.cancel());
            } catch (...) {
            }
            public_socket_.close(ignored);
        }

        void finish() noexcept {
            if (finished_) {
                return;
            }
            finished_ = true;
            try {
                static_cast<void>(deadline_timer_.cancel());
            } catch (...) {
            }
            server_->pending_connection_finished(this);
        }

        std::shared_ptr<Impl> server_;
        TunnelBinding binding_;
        std::string session_key_;
        asio::ip::tcp::socket public_socket_;
        asio::steady_timer deadline_timer_;
        ConnectionQuota::Lease connection_lease_;
        bool finished_{false};
    };

    Impl(asio::io_context& io_context, ServerOptions options,
         const asio::ip::tcp::endpoint listen_endpoint,
         std::shared_ptr<asio::ssl::context> tls_context, common::SecureString token,
         std::string server_id, common::PortRange allowed_ports)
        : io_context_(io_context), options_(std::move(options)),
          strand_(asio::make_strand(io_context)), acceptor_(strand_), accept_retry_timer_(strand_),
          shutdown_timer_(strand_), listen_endpoint_(listen_endpoint),
          tls_context_(std::move(tls_context)), token_(std::move(token)),
          server_id_(std::move(server_id)), session_registry_(options_.max_clients),
          worker_pool_(options_.max_idle_workers, options_.max_total_idle_workers),
          connection_quota_(options_.max_connections_per_client, options_.max_total_connections),
          tunnel_registry_(
              strand_, io_context_.get_executor(), std::move(allowed_ports),
              options_.max_tunnels_per_client,
              [this](TunnelBinding binding, asio::ip::tcp::socket public_socket) mutable {
                  handle_public_connection(std::move(binding), std::move(public_socket));
              }) {}

    void handle_public_connection(TunnelBinding binding, asio::ip::tcp::socket public_socket) {
        if (!running_.load()) {
            asio::error_code ignored;
            public_socket.close(ignored);
            return;
        }
        auto connection_lease = connection_quota_.try_acquire(binding.client_id);
        if (!connection_lease) {
            asio::error_code ignored;
            public_socket.close(ignored);
            common::log_warn("public connection quota rejected a relay",
                             {.component = "server.relay",
                              .server_id = server_id_,
                              .tunnel_id = binding.tunnel_id,
                              .error_code = connection_lease.error().code()});
            return;
        }
        auto pending = std::make_shared<PendingPublicConnection>(
            shared_from_this(), std::move(binding), std::move(public_socket),
            std::move(*connection_lease));
        pending_connections_.emplace(pending.get(), pending);
        auto& queue = pending_connections_by_session_[pending->session_key()];
        const bool first_waiter = queue.empty();
        queue.push_back(pending.get());
        pending_connection_positions_.emplace(pending.get(), std::prev(queue.end()));
        pending->start(first_waiter);
    }

    void pending_connection_finished(PendingPublicConnection* pending) noexcept {
        const auto group = pending_connections_by_session_.find(pending->session_key());
        const auto position = pending_connection_positions_.find(pending);
        if (group != pending_connections_by_session_.end() &&
            position != pending_connection_positions_.end()) {
            group->second.erase(position->second);
            pending_connection_positions_.erase(position);
            if (group->second.empty()) {
                pending_connections_by_session_.erase(group);
            }
        }
        pending_connections_.erase(pending);
    }

    void notify_worker_available(std::string client_id, const std::uint64_t session_generation) {
        auto self = shared_from_this();
        asio::post(strand_, [self, client_id = std::move(client_id), session_generation] {
            const std::string session_key = pending_session_key(client_id, session_generation);
            for (;;) {
                auto group = self->pending_connections_by_session_.find(session_key);
                if (group == self->pending_connections_by_session_.end() || group->second.empty()) {
                    return;
                }
                PendingPublicConnection* candidate = group->second.front();
                const auto owned = self->pending_connections_.find(candidate);
                if (owned == self->pending_connections_.end()) {
                    group->second.pop_front();
                    self->pending_connection_positions_.erase(candidate);
                    if (group->second.empty()) {
                        self->pending_connections_by_session_.erase(group);
                    }
                    continue;
                }
                auto pending = owned->second;
                if (pending->try_assign()) {
                    return;
                }
                // A failed assignment either removed an obsolete pending connection
                // or found that another notification already consumed the worker.
                // Continue only in the former case.
                if (self->pending_connections_.contains(candidate)) {
                    return;
                }
            }
        });
    }

    [[nodiscard]] static std::string pending_session_key(const std::string_view client_id,
                                                         const std::uint64_t session_generation) {
        std::string key{client_id};
        key.push_back('/');
        key.append(std::to_string(session_generation));
        return key;
    }

    void session_finished(std::shared_ptr<ControlSession> session) noexcept {
        auto self = shared_from_this();
        asio::post(strand_, [self, session = std::move(session)]() mutable {
            const auto owned = self->sessions_.find(session.get());
            if (owned != self->sessions_.end()) {
                session->cleanup_on_control_strand();
                self->sessions_.erase(owned);
                if (self->shutting_down_ && self->sessions_.empty()) {
                    try {
                        static_cast<void>(self->shutdown_timer_.cancel());
                    } catch (...) {
                    }
                }
            }

            // The TLS stream and its timers belong to the session strand. Keep
            // one owner alive until that strand runs again so their final
            // destruction cannot race a completion handler on another worker.
            auto session_executor = session->executor();
            asio::post(std::move(session_executor), [session = std::move(session)] {});
        });
    }

    void begin_shutdown() noexcept {
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        asio::error_code ignored;
        try {
            static_cast<void>(accept_retry_timer_.cancel());
        } catch (...) {
        }
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
        reserved_descriptor_.close();
        tunnel_registry_.clear();

        auto pending = std::move(pending_connections_);
        pending_connections_.clear();
        for (auto& [key, connection] : pending) {
            static_cast<void>(key);
            connection->stop();
        }

        worker_pool_.clear();
        for (auto& [key, session] : sessions_) {
            static_cast<void>(key);
            if (!session->relay_active()) {
                session->request_stop(true);
            }
        }
        if (sessions_.empty()) {
            return;
        }

        shutdown_timer_.expires_after(options_.graceful_shutdown_timeout);
        auto self = shared_from_this();
        shutdown_timer_.async_wait([self](const asio::error_code& error) {
            if (!error) {
                self->force_shutdown();
            }
        });
    }

    void force_shutdown() noexcept {
        for (auto& [key, session] : sessions_) {
            static_cast<void>(key);
            session->force_stop();
        }
    }

    void accept_next() {
        if (!running_.load() || !acceptor_.is_open()) {
            return;
        }
        auto self = shared_from_this();
        acceptor_.async_accept(
            asio::make_strand(io_context_),
            asio::bind_executor(strand_, [self](const asio::error_code& error,
                                                asio::ip::tcp::socket socket) mutable {
                if (!error && self->running_.load()) {
                    self->accept_retry_policy_.reset();
                    const std::size_t previous = self->active_connections_.fetch_add(1U);
                    const std::size_t connection_limit =
                        std::min(kMaxServerConnections, self->options_.max_clients +
                                                            self->options_.max_total_idle_workers +
                                                            self->options_.max_total_connections);
                    if (previous < connection_limit) {
                        auto session = std::make_shared<ControlSession>(std::move(socket), self);
                        self->sessions_.emplace(session.get(), session);
                        session->start();
                    } else {
                        self->active_connections_.fetch_sub(1U);
                        asio::error_code ignored;
                        socket.close(ignored);
                    }
                } else if (AcceptRetryPolicy::retryable(error) && self->running_.load()) {
                    self->handle_accept_failure(error);
                    return;
                }
                if (self->running_.load()) {
                    self->accept_next();
                }
            }));
    }

    void handle_accept_failure(const asio::error_code& error) {
        if (AcceptRetryPolicy::descriptor_exhausted(error)) {
            reserved_descriptor_.recover(acceptor_);
        }
        if (accept_retry_policy_.should_log(AcceptRetryPolicy::Clock::now())) {
            common::log_warn(
                "TLS listener accept failed; retrying with backoff",
                {.component = "server.listener",
                 .server_id = server_id_,
                 .error_code =
                     AcceptRetryPolicy::resource_exhausted(error)
                         ? std::optional<common::ErrorCode>{common::ErrorCode::resource_exhausted}
                         : std::optional<common::ErrorCode>{common::ErrorCode::connection_failed}});
        }
        accept_retry_timer_.expires_after(accept_retry_policy_.next_delay());
        auto self = shared_from_this();
        accept_retry_timer_.async_wait([self](const asio::error_code& timer_error) {
            if (!timer_error && self->running_.load() && self->acceptor_.is_open()) {
                self->accept_next();
            }
        });
    }

    asio::io_context& io_context_;
    ServerOptions options_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer accept_retry_timer_;
    asio::steady_timer shutdown_timer_;
    asio::ip::tcp::endpoint listen_endpoint_;
    std::shared_ptr<asio::ssl::context> tls_context_;
    common::SecureString token_;
    std::string server_id_;
    protocol::NonceReplayCache nonce_cache_;
    protocol::AuthRateLimiter auth_rate_limiter_;
    SessionRegistry session_registry_;
    WorkerPool worker_pool_;
    ConnectionQuota connection_quota_;
    TunnelRegistry tunnel_registry_;
    std::unordered_map<ControlSession*, std::shared_ptr<ControlSession>> sessions_;
    std::unordered_map<PendingPublicConnection*, std::shared_ptr<PendingPublicConnection>>
        pending_connections_;
    using PendingConnectionQueue = std::list<PendingPublicConnection*>;
    std::unordered_map<std::string, PendingConnectionQueue> pending_connections_by_session_;
    std::unordered_map<PendingPublicConnection*, PendingConnectionQueue::iterator>
        pending_connection_positions_;
    std::atomic<std::size_t> active_connections_{0U};
    std::atomic<std::uint16_t> listening_port_{0U};
    std::atomic<bool> running_{false};
    ReservedFileDescriptor reserved_descriptor_;
    AcceptRetryPolicy accept_retry_policy_;
    bool shutting_down_{false};
};

common::Result<std::unique_ptr<Server>> Server::create(asio::io_context& io_context,
                                                       ServerOptions options) {
    auto implementation = Impl::create(io_context, std::move(options));
    if (!implementation) {
        return common::Result<std::unique_ptr<Server>>::failure(implementation.error());
    }
    return std::unique_ptr<Server>{new Server{std::move(*implementation)}};
}

Server::Server(std::shared_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

Server::~Server() noexcept { stop(); }

common::Result<void> Server::start() { return implementation_->start(); }

void Server::stop() noexcept { implementation_->stop(); }

std::uint16_t Server::listening_port() const noexcept { return implementation_->listening_port(); }

const std::string& Server::server_id() const noexcept { return implementation_->server_id(); }

} // namespace minitun::server
