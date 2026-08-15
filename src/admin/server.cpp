#include <minitun/admin/server.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/secure_string.hpp>

namespace minitun::admin {
namespace {

using asio::ip::tcp;
using common::Error;
using common::ErrorCode;
using common::Result;

constexpr std::size_t kMaximumTokenBytes = 4U * 1024U;

[[nodiscard]] Result<common::SecureString> read_token_file(const std::string& path) {
    if (path.empty() || path.size() > 4'096U || path.find('\0') != std::string::npos) {
        return Error{ErrorCode::invalid_argument, "admin token file path is invalid"};
    }
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return Error{errno == EACCES ? ErrorCode::permission_denied : ErrorCode::invalid_argument,
                     "admin token file cannot be opened"};
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0 || status.st_size <= 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaximumTokenBytes) {
        static_cast<void>(::close(descriptor));
        return Error{ErrorCode::permission_denied,
                     "admin token file must be private, owned, and bounded"};
    }
    std::string token(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0U;
    while (offset < token.size()) {
        const ssize_t count = ::read(descriptor, token.data() + offset, token.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            static_cast<void>(::close(descriptor));
            common::secure_erase_memory(token.data(), token.size());
            return Error{ErrorCode::invalid_argument, "admin token file could not be read"};
        }
        offset += static_cast<std::size_t>(count);
    }
    static_cast<void>(::close(descriptor));
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
    }
    if (token.empty() || token.find('\0') != std::string::npos) {
        common::secure_erase_memory(token.data(), token.size());
        return Error{ErrorCode::invalid_argument, "admin token is empty or invalid"};
    }
    common::SecureString secured{token};
    common::secure_erase_memory(token.data(), token.size());
    return secured;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

struct Request final {
    std::string method;
    std::string path;
    std::optional<std::string> authorization;
    std::size_t content_length{0U};

    ~Request() {
        if (authorization.has_value()) {
            common::secure_erase_memory(authorization->data(), authorization->size());
        }
    }
};

[[nodiscard]] Result<Request> parse_request(const std::string_view text) {
    const auto header_end = text.find("\r\n\r\n");
    if (header_end == std::string_view::npos || header_end + 4U != text.size()) {
        return Error{ErrorCode::invalid_argument, "admin HTTP request framing is invalid"};
    }
    const auto first_end = text.find("\r\n");
    if (first_end == std::string_view::npos) {
        return Error{ErrorCode::invalid_argument, "admin HTTP request line is invalid"};
    }
    const std::string_view first = text.substr(0U, first_end);
    const auto first_space = first.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : first.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
        first.find(' ', second_space + 1U) != std::string_view::npos ||
        first.substr(second_space + 1U) != "HTTP/1.1") {
        return Error{ErrorCode::invalid_argument, "admin HTTP request line is invalid"};
    }
    Request request{std::string{first.substr(0U, first_space)},
                    std::string{first.substr(first_space + 1U, second_space - first_space - 1U)},
                    std::nullopt, 0U};
    if (request.path.empty() || request.path.front() != '/' || request.path.find('?') != std::string::npos) {
        return Error{ErrorCode::invalid_argument, "admin HTTP request target is invalid"};
    }
    std::size_t cursor = first_end + 2U;
    std::size_t header_count = 0U;
    bool content_length_seen = false;
    while (cursor < header_end) {
        const auto line_end = text.find("\r\n", cursor);
        if (line_end == std::string_view::npos || line_end > header_end || ++header_count > 64U) {
            return Error{ErrorCode::invalid_argument, "admin HTTP headers are invalid"};
        }
        const std::string_view line = text.substr(cursor, line_end - cursor);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return Error{ErrorCode::invalid_argument, "admin HTTP header is invalid"};
        }
        const std::string name = lower_ascii(std::string{line.substr(0U, colon)});
        std::string_view value = line.substr(colon + 1U);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1U);
        }
        if (name == "content-length") {
            if (content_length_seen || value.empty() ||
                !std::all_of(value.begin(), value.end(), [](const char byte) {
                    return byte >= '0' && byte <= '9';
                })) {
                return Error{ErrorCode::invalid_argument,
                             "admin HTTP content-length header is invalid"};
            }
            content_length_seen = true;
            std::size_t parsed = 0U;
            for (const char byte : value) {
                const unsigned digit = static_cast<unsigned>(byte - '0');
                if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
                    return Error{ErrorCode::invalid_argument,
                                 "admin HTTP content-length overflows"};
                }
                parsed = parsed * 10U + digit;
            }
            request.content_length = parsed;
        } else if (name == "transfer-encoding") {
            return Error{ErrorCode::invalid_argument, "admin HTTP transfer encoding is forbidden"};
        } else if (name == "authorization") {
            if (request.authorization.has_value()) {
                return Error{ErrorCode::invalid_argument,
                             "duplicate admin authorization header"};
            }
            request.authorization = std::string{value};
        }
        cursor = line_end + 2U;
    }
    return request;
}

[[nodiscard]] std::pair<unsigned int, std::string_view>
status_for_error(const common::ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::not_found:
        return {404U, "Not Found"};
    case ErrorCode::already_exists:
        return {409U, "Conflict"};
    case ErrorCode::permission_denied:
        return {403U, "Forbidden"};
    case ErrorCode::not_authenticated:
    case ErrorCode::authentication_failed:
        return {401U, "Unauthorized"};
    case ErrorCode::resource_exhausted:
        return {413U, "Content Too Large"};
    case ErrorCode::invalid_argument:
    case ErrorCode::protocol_error:
    case ErrorCode::unsupported_version:
        return {400U, "Bad Request"};
    default:
        return {500U, "Internal Server Error"};
    }
}

[[nodiscard]] std::string response(const unsigned int status, const std::string_view reason,
                                   const std::string_view content_type, const std::string_view body,
                                   const bool head) {
    std::string result = "HTTP/1.1 " + std::to_string(status) + " " + std::string{reason} + "\r\n";
    result.append("Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n");
    result.append("Content-Type: ").append(content_type).append("\r\nContent-Length: ");
    result.append(std::to_string(body.size())).append("\r\n\r\n");
    if (!head) {
        result.append(body);
    }
    return result;
}

} // namespace

class Server::Impl final : public std::enable_shared_from_this<Server::Impl> {
  public:
    Impl(asio::io_context& io_context, tcp::endpoint endpoint, ServerOptions options,
        Providers providers, std::shared_ptr<const common::SecureString> token,
         const bool authentication_required)
        : strand_(asio::make_strand(io_context)), acceptor_(strand_),
          endpoint_(std::move(endpoint)),
          options_(std::move(options)), providers_(std::move(providers)), token_(std::move(token)),
          authentication_required_(authentication_required) {}

    [[nodiscard]] Result<void> start() {
        if (running_.exchange(true)) {
            return Error{ErrorCode::already_exists, "admin listener is already running"};
        }
        asio::error_code error;
        acceptor_.open(endpoint_.protocol(), error);
        if (!error) {
            acceptor_.set_option(tcp::acceptor::reuse_address(true), error);
        }
        if (!error) {
            acceptor_.bind(endpoint_, error);
        }
        if (!error) {
            acceptor_.listen(static_cast<int>(std::min<std::size_t>(options_.max_connections, 128U)),
                             error);
        }
        if (error) {
            running_.store(false);
            return Error{ErrorCode::connection_failed, "admin listener could not bind"};
        }
        accept_next();
        return Result<void>::success();
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
        asio::error_code error;
        const auto endpoint = acceptor_.local_endpoint(error);
        return error ? 0U : endpoint.port();
    }

  private:
    class Session final : public std::enable_shared_from_this<Session> {
      public:
        Session(std::shared_ptr<Impl> owner, tcp::socket socket)
            : owner_(std::move(owner)), socket_(std::move(socket)), timer_(socket_.get_executor()),
              buffer_(owner_->options_.max_header_bytes) {}

        void start() {
            timer_.expires_after(owner_->options_.timeout);
            auto self = shared_from_this();
            timer_.async_wait([self](const asio::error_code& error) {
                if (!error) {
                    self->close();
                }
            });
            asio::async_read_until(socket_, buffer_, "\r\n\r\n",
                                   [self](const asio::error_code& error, const std::size_t bytes) {
                                       self->on_headers(error, bytes);
                                   });
        }

      private:
        void on_headers(const asio::error_code& error, const std::size_t bytes) {
            if (error || bytes > owner_->options_.max_header_bytes) {
                write_response(response(431U, "Request Header Fields Too Large", "text/plain",
                                        "request headers rejected\n", false));
                return;
            }
            std::string text(bytes, '\0');
            {
                std::istream input{&buffer_};
                input.read(text.data(), static_cast<std::streamsize>(text.size()));
            }
            auto request = parse_request(text);
            common::secure_erase_memory(text.data(), text.size());
            if (!request) {
                write_response(response(400U, "Bad Request", "text/plain", "bad request\n", false));
                return;
            }
            if ((request->method == "GET" || request->method == "HEAD") &&
                request->content_length != 0U) {
                write_response(response(400U, "Bad Request", "text/plain",
                                        "GET requests cannot carry a body\n", false));
                return;
            }
            if (request->content_length > owner_->options_.max_body_bytes) {
                write_response(response(413U, "Content Too Large", "text/plain",
                                        "request body rejected\n", false));
                return;
            }
            const std::size_t buffered = buffer_.size();
            if (buffered > request->content_length) {
                write_response(response(400U, "Bad Request", "text/plain",
                                        "request pipelining is forbidden\n", false));
                return;
            }
            body_.reserve(request->content_length);
            if (buffered != 0U) {
                const auto previous = body_.size();
                body_.resize(previous + buffered);
                std::istream input{&buffer_};
                input.read(body_.data() + static_cast<std::ptrdiff_t>(previous),
                           static_cast<std::streamsize>(buffered));
            }
            if (body_.size() < request->content_length) {
                pending_ = std::move(*request);
                auto self = shared_from_this();
                asio::async_read(socket_, buffer_,
                                 asio::transfer_exactly(request->content_length - body_.size()),
                                 [self](const asio::error_code& read_error, const std::size_t) {
                                     self->on_body(read_error);
                                 });
                return;
            }
            dispatch(std::move(*request));
        }

        void on_body(const asio::error_code& error) {
            if (error) {
                write_response(response(400U, "Bad Request", "text/plain",
                                        "request body truncated\n", false));
                return;
            }
            const std::size_t buffered = buffer_.size();
            const auto previous = body_.size();
            body_.resize(previous + buffered);
            {
                std::istream input{&buffer_};
                input.read(body_.data() + static_cast<std::ptrdiff_t>(previous),
                           static_cast<std::streamsize>(buffered));
            }
            if (body_.size() != pending_->content_length) {
                write_response(response(400U, "Bad Request", "text/plain",
                                        "request body length mismatch\n", false));
                return;
            }
            dispatch(std::move(*pending_));
        }

        void dispatch(Request request) {
            if (owner_->providers_.management && request.path.starts_with("/v1/")) {
                dispatch_management(std::move(request));
                return;
            }
            if (owner_->authentication_required_ && !authenticated(request)) {
                write_unauthorized();
                return;
            }
            const bool head = request.method == "HEAD";
            if (request.method != "GET" && !head) {
                write_response(response(405U, "Method Not Allowed", "text/plain",
                                        "method not allowed\n", false));
                return;
            }
            if (request.path == "/healthz") {
                const bool healthy = safe_status(owner_->providers_.healthy);
                write_response(response(healthy ? 200U : 503U,
                                        healthy ? "OK" : "Service Unavailable", "text/plain",
                                        healthy ? "ok\n" : "unhealthy\n", head));
            } else if (request.path == "/readyz") {
                const bool ready = safe_status(owner_->providers_.ready);
                write_response(response(ready ? 200U : 503U,
                                        ready ? "OK" : "Service Unavailable", "text/plain",
                                        ready ? "ready\n" : "not ready\n", head));
            } else if (request.path == "/metrics") {
                if (head) {
                    write_response(response(405U, "Method Not Allowed", "text/plain",
                                            "method not allowed\n", true));
                    return;
                }
                std::string metrics;
                try {
                    metrics = owner_->providers_.metrics ? owner_->providers_.metrics() : std::string{};
                } catch (...) {
                    write_response(response(500U, "Internal Server Error", "text/plain",
                                            "metrics unavailable\n", false));
                    return;
                }
                write_response(response(200U, "OK", "text/plain; version=0.0.4; charset=utf-8",
                                        metrics, false));
            } else {
                write_response(response(404U, "Not Found", "text/plain", "not found\n", head));
            }
        }

        void dispatch_management(Request request) {
            if (owner_->token_ != nullptr && !authenticated(request)) {
                write_unauthorized();
                return;
            }
            if (request.method != "GET" && request.method != "PUT" &&
                request.method != "POST" && request.method != "DELETE") {
                write_response(response(405U, "Method Not Allowed", "text/plain",
                                        "method not allowed\n", false));
                return;
            }
            ManagementRequest management{request.method, request.path, std::move(body_)};
            const auto invoke = [this, &management]() -> common::Result<ManagementResponse> {
                try {
                    return owner_->providers_.management(management);
                } catch (...) {
                    return common::Result<ManagementResponse>::failure(
                        common::ErrorCode::internal_error, "management handler failed");
                }
            };
            const auto result = invoke();
            if (!result) {
                const auto [status, reason] = status_for_error(result.error().code());
                std::string body = "{\"error\":\"" + std::string{common::to_string(result.error().code())} +
                                   "\",\"message\":\"" + result.error().message() + "\"}\n";
                write_response(response(status, reason, "application/json", body, false));
                return;
            }
            write_response(response(result->status, result->reason, result->content_type,
                                    result->body, false));
        }

        void write_unauthorized() {
            auto denied = response(401U, "Unauthorized", "text/plain", "unauthorized\n", false);
            denied.insert(denied.find("\r\n") + 2U,
                          "WWW-Authenticate: Bearer realm=\"minitun-admin\"\r\n");
            write_response(std::move(denied));
        }

        [[nodiscard]] bool authenticated(const Request& request) const {
            constexpr std::string_view prefix{"Bearer "};
            if (!request.authorization.has_value() ||
                !request.authorization->starts_with(prefix)) {
                return false;
            }
            try {
                common::SecureString supplied{
                    std::string_view{*request.authorization}.substr(prefix.size())};
                return owner_->token_ != nullptr && owner_->token_->equals(supplied);
            } catch (...) {
                return false;
            }
        }

        [[nodiscard]] static bool safe_status(const std::function<bool()>& provider) noexcept {
            try {
                return provider && provider();
            } catch (...) {
                return false;
            }
        }

        void write_response(std::string message) {
            response_ = std::move(message);
            auto self = shared_from_this();
            asio::async_write(socket_, asio::buffer(response_),
                              [self](const asio::error_code&, const std::size_t) { self->close(); });
        }

        void close() noexcept {
            asio::error_code ignored;
            try {
                static_cast<void>(timer_.cancel());
            } catch (...) {
            }
            socket_.shutdown(tcp::socket::shutdown_both, ignored);
            socket_.close(ignored);
            if (!finished_.exchange(true)) {
                owner_->active_connections_.fetch_sub(1U);
            }
        }

        std::shared_ptr<Impl> owner_;
        tcp::socket socket_;
        asio::steady_timer timer_;
        asio::streambuf buffer_;
        std::string response_;
        std::string body_;
        std::optional<Request> pending_;
        std::atomic_bool finished_{false};
    };

    void accept_next() {
        if (!running_.load()) {
            return;
        }
        auto self = shared_from_this();
        acceptor_.async_accept([self](const asio::error_code& error, tcp::socket socket) {
            if (!error && self->running_.load()) {
                const std::size_t active = self->active_connections_.fetch_add(1U) + 1U;
                if (active <= self->options_.max_connections) {
                    std::make_shared<Session>(self, std::move(socket))->start();
                } else {
                    self->active_connections_.fetch_sub(1U);
                    asio::error_code ignored;
                    socket.close(ignored);
                }
            }
            self->accept_next();
        });
    }

    asio::strand<asio::io_context::executor_type> strand_;
    tcp::acceptor acceptor_;
    tcp::endpoint endpoint_;
    ServerOptions options_;
    Providers providers_;
    std::shared_ptr<const common::SecureString> token_;
    bool authentication_required_{false};
    std::atomic_bool running_{false};
    std::atomic_size_t active_connections_{0U};
};

Result<std::unique_ptr<Server>> Server::create(asio::io_context& io_context, ServerOptions options,
                                               Providers providers) {
    if (options.listen_endpoint.empty() || options.max_connections == 0U ||
        options.max_connections > 1'024U || options.max_header_bytes < 1'024U ||
        options.max_header_bytes > 64U * 1024U || options.max_body_bytes < 1'024U ||
        options.max_body_bytes > 1024U * 1024U || options.timeout <= std::chrono::seconds::zero() ||
        options.timeout > std::chrono::seconds{60}) {
        return Error{ErrorCode::invalid_argument, "admin listener options are invalid"};
    }
    auto parsed = common::Endpoint::parse(options.listen_endpoint);
    if (!parsed) {
        return parsed.error();
    }
    asio::error_code address_error;
    const auto address = asio::ip::make_address(parsed->host(), address_error);
    if (address_error) {
        return Error{ErrorCode::invalid_argument,
                     "admin listener host must be a numeric IP address"};
    }
    const bool authentication_required = !address.is_loopback();
    std::shared_ptr<const common::SecureString> token;
    if (!options.token_file.empty()) {
        auto loaded = read_token_file(options.token_file);
        if (!loaded) {
            return loaded.error();
        }
        token = std::make_shared<const common::SecureString>(std::move(*loaded));
    }
    if (authentication_required && token == nullptr) {
        return Error{ErrorCode::permission_denied,
                     "a non-loopback admin listener requires --admin-token-file"};
    }
    auto implementation = std::make_shared<Impl>(
        io_context, tcp::endpoint{address, parsed->port()}, std::move(options), std::move(providers),
        std::move(token), authentication_required);
    return std::unique_ptr<Server>{new Server{std::move(implementation)}};
}

Server::Server(std::shared_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
Server::~Server() noexcept { stop(); }
Result<void> Server::start() { return implementation_->start(); }
void Server::stop() noexcept { implementation_->stop(); }
std::uint16_t Server::listening_port() const noexcept { return implementation_->listening_port(); }

} // namespace minitun::admin
