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
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
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
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/protocol/auth.hpp>
#include <minitun/protocol/messages.hpp>
#include <minitun/protocol/state_machine.hpp>
#include <minitun/protocol/tls.hpp>
#include <minitun/server/session_registry.hpp>

namespace minitun::server {
namespace {

inline constexpr std::size_t kMaxTokenFileBytes = 64U * 1024U;
inline constexpr std::size_t kMaxServerConnections = 100'000U;
inline constexpr std::chrono::seconds kMaxConfiguredTimeout{300};

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

[[nodiscard]] common::Result<common::SecureString>
load_token_file(const std::string& path) {
    if (path.empty() || path.size() > 4'096U || path.find('\0') != std::string::npos) {
        return common::Result<common::SecureString>::failure(
            common::ErrorCode::invalid_argument, "Token file path is invalid");
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

    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return common::Result<common::SecureString>::failure(
            common::ErrorCode::permission_denied,
            "Token file must be a regular file");
    }
    if (status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
        return common::Result<common::SecureString>::failure(
            common::ErrorCode::permission_denied,
            "Token file must be owned by the server user and inaccessible to group and others");
    }
    if (status.st_size <= 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaxTokenFileBytes) {
        return common::Result<common::SecureString>::failure(
            common::ErrorCode::invalid_argument, "Token file size is invalid");
    }

    std::vector<char> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
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
        return common::Result<common::SecureString>::failure(
            common::ErrorCode::invalid_argument, "Token file contains an empty Token");
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
    if (options.handshake_timeout <= std::chrono::seconds::zero() ||
        options.handshake_timeout > kMaxConfiguredTimeout ||
        options.heartbeat_interval <= std::chrono::seconds::zero() ||
        options.heartbeat_interval > kMaxConfiguredTimeout ||
        options.heartbeat_timeout <= options.heartbeat_interval ||
        options.heartbeat_timeout > kMaxConfiguredTimeout ||
        options.allowed_clock_skew < std::chrono::seconds::zero() ||
        options.allowed_clock_skew > kMaxConfiguredTimeout) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server timeout configuration is invalid");
    }
    if (options.min_idle_workers > options.max_idle_workers ||
        options.max_idle_workers > 128U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "server worker limits are invalid");
    }
    return common::Result<void>::success();
}

} // namespace

class Server::Impl final : public std::enable_shared_from_this<Server::Impl> {
  public:
    [[nodiscard]] static common::Result<std::shared_ptr<Impl>>
    create(asio::io_context& io_context, ServerOptions options) {
        auto valid = validate_options(options);
        if (!valid) {
            return common::Result<std::shared_ptr<Impl>>::failure(valid.error());
        }
        auto endpoint = common::Endpoint::parse(options.listen_endpoint);
        if (!endpoint) {
            return common::Result<std::shared_ptr<Impl>>::failure(endpoint.error());
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
                                              server_id->str())};
    }

    ~Impl() noexcept { stop(); }

    [[nodiscard]] common::Result<void> start() {
        if (running_.exchange(true)) {
            return common::Result<void>::failure(common::ErrorCode::already_exists,
                                                 "TLS server is already running");
        }

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
        asio::error_code ignored;
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
    }

    [[nodiscard]] std::uint16_t listening_port() const noexcept {
        return listening_port_.load();
    }

    [[nodiscard]] const std::string& server_id() const noexcept { return server_id_; }

  private:
    class ControlSession final : public std::enable_shared_from_this<ControlSession> {
      public:
        ControlSession(asio::ip::tcp::socket socket, std::shared_ptr<Impl> server)
            : server_(std::move(server)), stream_(std::move(socket), *server_->tls_context_),
              operation_timer_(stream_.get_executor()),
              heartbeat_timer_(stream_.get_executor()),
              state_(protocol::PeerRole::server, protocol::ConnectionKind::control) {
            asio::error_code endpoint_error;
            const auto endpoint = stream_.lowest_layer().remote_endpoint(endpoint_error);
            remote_endpoint_ = endpoint_error ? std::string{} : endpoint.address().to_string();
            auto connection_id = common::Id::generate(common::IdKind::connection);
            if (connection_id) {
                connection_id_ = connection_id->str();
            }
        }

        ~ControlSession() {
            if (generation_ != 0U && !client_id_.empty()) {
                server_->session_registry_.close(client_id_, generation_);
            }
            server_->active_connections_.fetch_sub(1U);
            protocol::close_tls_stream(stream_);
        }

        void start() {
            auto self = shared_from_this();
            asio::co_spawn(
                stream_.get_executor(), run(),
                [self](const std::exception_ptr failure) {
                    if (failure) {
                        common::log_warn("remote control session ended with an exception",
                                         self->log_context(common::ErrorCode::internal_error));
                    }
                    self->cancel_timers();
                    protocol::close_tls_stream(self->stream_);
                });
        }

      private:
        [[nodiscard]] asio::awaitable<void> run() {
            if (!co_await perform_tls_handshake()) {
                co_return;
            }
            if (!co_await authenticate()) {
                co_return;
            }
            common::log_info("remote client authenticated", log_context());
            co_await heartbeat_loop();
        }

        [[nodiscard]] asio::awaitable<bool> perform_tls_handshake() {
            arm_operation_timeout(server_->options_.handshake_timeout);
            asio::error_code error;
            co_await stream_.async_handshake(
                asio::ssl::stream_base::server,
                asio::redirect_error(asio::use_awaitable, error));
            cancel_operation_timeout();
            if (error) {
                common::log_warn("TLS handshake failed", log_context(common::ErrorCode::tls_error));
                co_return false;
            }
            co_return true;
        }

        [[nodiscard]] asio::awaitable<bool> authenticate() {
            auto hello_frame = co_await read_frame(server_->options_.handshake_timeout);
            if (!hello_frame || hello_frame->type != protocol::MessageType::hello) {
                co_return false;
            }
            auto hello = protocol::decode_hello(hello_frame->payload);
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
            if (!ack_payload ||
                !co_await write_frame({protocol::MessageType::hello_ack, 0U,
                                       hello_frame->request_id, std::move(*ack_payload)},
                                      server_->options_.handshake_timeout)) {
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
                    server_->token_.view(), auth->client_id, auth->timestamp_seconds,
                    auth->nonce, auth->authentication_data);
                accepted = verified && *verified;
            }
            if (!accepted) {
                server_->auth_rate_limiter_.record_failure(remote_endpoint_, now);
                co_return co_await reject_authentication(auth_frame->request_id);
            }

            auto generation = server_->session_registry_.open(client_id_);
            if (!generation) {
                auto error_payload = protocol::encode_auth_error(
                    {common::ErrorCode::resource_exhausted});
                if (error_payload) {
                    static_cast<void>(co_await write_frame(
                        {protocol::MessageType::auth_error, 0U, auth_frame->request_id,
                         std::move(*error_payload)},
                        server_->options_.handshake_timeout));
                }
                co_return false;
            }
            generation_ = *generation;

            const auto heartbeat_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                server_->options_.heartbeat_interval);
            auto ok_payload = protocol::encode_auth_ok({
                .session_generation = generation_,
                .heartbeat_interval_milliseconds =
                    static_cast<std::uint32_t>(heartbeat_milliseconds.count()),
                .min_idle_workers = server_->options_.min_idle_workers,
                .max_idle_workers = server_->options_.max_idle_workers,
            });
            if (!ok_payload ||
                !co_await write_frame({protocol::MessageType::auth_ok, 0U,
                                       auth_frame->request_id, std::move(*ok_payload)},
                                      server_->options_.handshake_timeout)) {
                co_return false;
            }
            server_->auth_rate_limiter_.record_success(remote_endpoint_);
            co_return true;
        }

        [[nodiscard]] asio::awaitable<bool>
        reject_authentication(const std::uint64_t request_id) {
            auto payload = protocol::encode_auth_error(
                {common::ErrorCode::authentication_failed});
            if (payload) {
                static_cast<void>(co_await write_frame(
                    {protocol::MessageType::auth_error, 0U, request_id, std::move(*payload)},
                    server_->options_.handshake_timeout));
            }
            common::log_warn("remote client authentication failed",
                             log_context(common::ErrorCode::authentication_failed));
            co_return false;
        }

        [[nodiscard]] asio::awaitable<void> heartbeat_loop() {
            std::uint64_t sequence = 1U;
            for (;;) {
                heartbeat_timer_.expires_after(server_->options_.heartbeat_interval);
                asio::error_code timer_error;
                co_await heartbeat_timer_.async_wait(
                    asio::redirect_error(asio::use_awaitable, timer_error));
                if (timer_error) {
                    co_return;
                }

                auto ping_payload = protocol::encode_heartbeat({sequence});
                if (!ping_payload ||
                    !co_await write_frame({protocol::MessageType::ping, 0U, sequence,
                                           std::move(*ping_payload)},
                                          server_->options_.heartbeat_timeout)) {
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
                    if (frame->type == protocol::MessageType::pong) {
                        auto pong = protocol::decode_heartbeat(frame->payload);
                        if (!pong || pong->sequence != sequence || frame->request_id != sequence) {
                            co_return;
                        }
                        received_pong = true;
                        continue;
                    }
                    if (frame->type == protocol::MessageType::ping) {
                        auto ping = protocol::decode_heartbeat(frame->payload);
                        if (!ping) {
                            co_return;
                        }
                        auto pong_payload = protocol::encode_heartbeat(*ping);
                        if (!pong_payload ||
                            !co_await write_frame({protocol::MessageType::pong, 0U,
                                                   frame->request_id,
                                                   std::move(*pong_payload)},
                                                  remaining)) {
                            co_return;
                        }
                        continue;
                    }
                    co_return;
                }
                ++sequence;
                if (sequence == 0U) {
                    sequence = 1U;
                }
            }
        }

        [[nodiscard]] asio::awaitable<common::Result<protocol::Frame>>
        read_frame(const std::chrono::seconds timeout) {
            arm_operation_timeout(timeout);
            auto frame = co_await protocol::async_read_frame(stream_);
            cancel_operation_timeout();
            if (!frame) {
                co_return frame;
            }
            auto transition = state_.on_receive(frame->type);
            if (!transition) {
                co_return common::Result<protocol::Frame>::failure(transition.error());
            }
            co_return frame;
        }

        [[nodiscard]] asio::awaitable<bool>
        write_frame(protocol::Frame frame, const std::chrono::seconds timeout) {
            auto transition = state_.on_send(frame.type);
            if (!transition) {
                co_return false;
            }
            arm_operation_timeout(timeout);
            auto written = co_await protocol::async_write_frame(stream_, frame);
            cancel_operation_timeout();
            co_return static_cast<bool>(written);
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
        protocol::StateMachine state_;
        std::string remote_endpoint_;
        std::string connection_id_;
        std::string client_id_;
        protocol::AuthenticationNonce challenge_nonce_{};
        std::uint64_t generation_{0U};
    };

    Impl(asio::io_context& io_context, ServerOptions options,
         const asio::ip::tcp::endpoint listen_endpoint,
         std::shared_ptr<asio::ssl::context> tls_context, common::SecureString token,
         std::string server_id)
        : options_(std::move(options)), strand_(asio::make_strand(io_context)),
          acceptor_(strand_), listen_endpoint_(listen_endpoint),
          tls_context_(std::move(tls_context)), token_(std::move(token)),
          server_id_(std::move(server_id)), session_registry_(options_.max_clients) {}

    void accept_next() {
        if (!running_.load()) {
            return;
        }
        auto self = shared_from_this();
        acceptor_.async_accept([self](const asio::error_code& error,
                                      asio::ip::tcp::socket socket) mutable {
            if (!error && self->running_.load()) {
                const std::size_t previous = self->active_connections_.fetch_add(1U);
                if (previous < self->options_.max_clients) {
                    std::make_shared<ControlSession>(std::move(socket), self)->start();
                } else {
                    self->active_connections_.fetch_sub(1U);
                    asio::error_code ignored;
                    socket.close(ignored);
                }
            }
            if (self->running_.load()) {
                self->accept_next();
            }
        });
    }

    ServerOptions options_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::endpoint listen_endpoint_;
    std::shared_ptr<asio::ssl::context> tls_context_;
    common::SecureString token_;
    std::string server_id_;
    protocol::NonceReplayCache nonce_cache_;
    protocol::AuthRateLimiter auth_rate_limiter_;
    SessionRegistry session_registry_;
    std::atomic<std::size_t> active_connections_{0U};
    std::atomic<std::uint16_t> listening_port_{0U};
    std::atomic<bool> running_{false};
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

std::uint16_t Server::listening_port() const noexcept {
    return implementation_->listening_port();
}

const std::string& Server::server_id() const noexcept { return implementation_->server_id(); }

} // namespace minitun::server
