#include <minitun/gui/server.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/thread_pool.hpp>
#include <asio/write.hpp>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/ipc/local_client.hpp>
#include <minitun/ipc/protocol.hpp>

namespace minitun::gui {
namespace {

using asio::ip::tcp;
using common::Error;
using common::ErrorCode;
using common::Result;
using Json = ipc::Json;

struct HttpRequest final {
    std::string method;
    std::string path;
    std::string host;
    std::optional<std::string> origin;
    std::string content_type;
    std::string body;
};

struct HttpResponse final {
    unsigned int status{200U};
    std::string reason{"OK"};
    std::string content_type{"application/json; charset=utf-8"};
    std::string body;
    std::string cache_control{"no-store"};
};

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] bool parse_decimal(const std::string_view value, std::size_t& output) noexcept {
    if (value.empty()) {
        return false;
    }
    output = 0U;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (output > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return false;
        }
        output = output * 10U + digit;
    }
    return true;
}

[[nodiscard]] Result<std::pair<HttpRequest, std::size_t>>
parse_headers(const std::string_view text, const std::size_t max_body_bytes) {
    const auto header_end = text.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return Error{ErrorCode::invalid_argument, "GUI HTTP headers are incomplete"};
    }
    const auto first_end = text.find("\r\n");
    if (first_end == std::string_view::npos) {
        return Error{ErrorCode::invalid_argument, "GUI HTTP request line is invalid"};
    }
    const std::string_view first = text.substr(0U, first_end);
    const auto first_space = first.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : first.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
        first.find(' ', second_space + 1U) != std::string_view::npos ||
        first.substr(second_space + 1U) != "HTTP/1.1") {
        return Error{ErrorCode::invalid_argument, "GUI HTTP request line is invalid"};
    }
    HttpRequest request{
        .method = std::string{first.substr(0U, first_space)},
        .path = std::string{first.substr(first_space + 1U, second_space - first_space - 1U)}};
    if (request.path.empty() || request.path.front() != '/' ||
        request.path.find_first_of("?#\\\0") != std::string::npos) {
        return Error{ErrorCode::invalid_argument, "GUI HTTP target is invalid"};
    }

    std::size_t content_length = 0U;
    bool content_length_seen = false;
    std::size_t cursor = first_end + 2U;
    std::size_t header_count = 0U;
    while (cursor < header_end) {
        const auto line_end = text.find("\r\n", cursor);
        if (line_end == std::string_view::npos || line_end > header_end || ++header_count > 64U) {
            return Error{ErrorCode::invalid_argument, "GUI HTTP headers are invalid"};
        }
        const std::string_view line = text.substr(cursor, line_end - cursor);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return Error{ErrorCode::invalid_argument, "GUI HTTP header is invalid"};
        }
        const std::string name = lower_ascii(std::string{line.substr(0U, colon)});
        const std::string_view value = trim(line.substr(colon + 1U));
        if (name == "host") {
            if (!request.host.empty()) {
                return Error{ErrorCode::invalid_argument, "duplicate GUI Host header"};
            }
            request.host = std::string{value};
        } else if (name == "origin") {
            if (request.origin.has_value()) {
                return Error{ErrorCode::invalid_argument, "duplicate GUI Origin header"};
            }
            request.origin = std::string{value};
        } else if (name == "content-type") {
            request.content_type = lower_ascii(std::string{value});
        } else if (name == "content-length") {
            if (content_length_seen || !parse_decimal(value, content_length) ||
                content_length > max_body_bytes) {
                return Error{ErrorCode::invalid_argument, "GUI request body is too large"};
            }
            content_length_seen = true;
        } else if (name == "transfer-encoding") {
            return Error{ErrorCode::invalid_argument, "GUI HTTP transfer encoding is unsupported"};
        }
        cursor = line_end + 2U;
    }
    if (request.host.empty()) {
        return Error{ErrorCode::invalid_argument, "GUI HTTP Host header is required"};
    }
    return std::pair<HttpRequest, std::size_t>{std::move(request), content_length};
}

[[nodiscard]] HttpResponse json_error(const unsigned int status, std::string reason,
                                      const ErrorCode code, std::string message) {
    Json body{{"error", Json{{"code", std::string{common::to_string(code)}},
                             {"message", std::move(message)}}}};
    return {status, std::move(reason), "application/json; charset=utf-8", body.dump(), "no-store"};
}

[[nodiscard]] std::string serialize_response(const HttpResponse& response, const bool head) {
    std::string result = "HTTP/1.1 " + std::to_string(response.status) + " " + response.reason +
                         "\r\nConnection: close\r\n";
    result.append("Cache-Control: ").append(response.cache_control).append("\r\n");
    result.append("Content-Security-Policy: default-src 'self'; connect-src 'self'; ")
        .append("img-src 'self' data:; style-src 'self'; script-src 'self'; ")
        .append("font-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; ")
        .append("frame-ancestors 'none'\r\n");
    result.append("X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n")
        .append("Referrer-Policy: no-referrer\r\nCross-Origin-Resource-Policy: same-origin\r\n")
        .append("Content-Type: ")
        .append(response.content_type)
        .append("\r\nContent-Length: ")
        .append(std::to_string(response.body.size()))
        .append("\r\n\r\n");
    if (!head) {
        result.append(response.body);
    }
    return result;
}

[[nodiscard]] bool valid_asset_path(const std::string_view path) noexcept {
    if (path.empty() || path.front() != '/' || path.find("..") != std::string_view::npos ||
        path.find('%') != std::string_view::npos || path.find('\\') != std::string_view::npos) {
        return false;
    }
    for (const char character : path) {
        const bool allowed = std::isalnum(static_cast<unsigned char>(character)) != 0 ||
                             character == '/' || character == '.' || character == '_' ||
                             character == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool path_is_within(const std::filesystem::path& directory,
                                  const std::filesystem::path& candidate) noexcept {
    auto directory_part = directory.begin();
    auto candidate_part = candidate.begin();
    while (directory_part != directory.end()) {
        if (candidate_part == candidate.end() || *directory_part != *candidate_part) {
            return false;
        }
        ++directory_part;
        ++candidate_part;
    }
    return true;
}

[[nodiscard]] bool is_json_content_type(const std::string_view value) noexcept {
    constexpr std::string_view json_type{"application/json"};
    return value == json_type || (value.size() > json_type.size() && value.starts_with(json_type) &&
                                  value[json_type.size()] == ';');
}

[[nodiscard]] std::string content_type_for(const std::filesystem::path& path) {
    const std::string extension = lower_ascii(path.extension().string());
    if (extension == ".html") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".js") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".woff2") {
        return "font/woff2";
    }
    return "application/octet-stream";
}

[[nodiscard]] Result<std::string> read_asset(const std::filesystem::path& path,
                                             const std::size_t maximum) {
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size > maximum) {
        return Error{filesystem_error ? ErrorCode::not_found : ErrorCode::resource_exhausted,
                     filesystem_error ? "GUI asset was not found" : "GUI asset is too large"};
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return Error{ErrorCode::not_found, "GUI asset was not found"};
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!input && !data.empty()) {
        return Error{ErrorCode::connection_failed, "GUI asset could not be read"};
    }
    return data;
}

[[nodiscard]] std::vector<std::string> path_segments(const std::string_view path) {
    std::vector<std::string> segments;
    std::size_t begin = 1U;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto length = end == std::string_view::npos ? path.size() - begin : end - begin;
        if (length != 0U) {
            segments.emplace_back(path.substr(begin, length));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return segments;
}

} // namespace

class Server::Impl final : public std::enable_shared_from_this<Server::Impl> {
  private:
    class Session final : public std::enable_shared_from_this<Session> {
      public:
        Session(std::shared_ptr<Impl> owner, tcp::socket socket)
            : owner_(std::move(owner)), socket_(std::move(socket)), timer_(socket_.get_executor()),
              buffer_(owner_->options_.max_header_bytes + owner_->options_.max_body_bytes) {}

        void start() {
            timer_.expires_after(owner_->options_.request_timeout);
            auto self = shared_from_this();
            timer_.async_wait([self](const asio::error_code& error) {
                if (!error) {
                    self->close();
                }
            });
            asio::async_read_until(
                socket_, buffer_, "\r\n\r\n",
                [self](const asio::error_code& error, const std::size_t header_bytes) {
                    self->on_headers(error, header_bytes);
                });
        }

      private:
        void on_headers(const asio::error_code& error, const std::size_t header_bytes) {
            if (error || header_bytes > owner_->options_.max_header_bytes) {
                reply(json_error(431U, "Request Header Fields Too Large",
                                 ErrorCode::invalid_argument, "request headers rejected"));
                return;
            }
            std::string header(header_bytes, '\0');
            std::istream input{&buffer_};
            input.read(header.data(), static_cast<std::streamsize>(header.size()));
            auto parsed = parse_headers(header, owner_->options_.max_body_bytes);
            if (!parsed) {
                reply(json_error(400U, "Bad Request", parsed.error().code(),
                                 parsed.error().message()));
                return;
            }
            request_ = std::move(parsed->first);
            expected_body_bytes_ = parsed->second;
            if (!owner_->accepts_host(request_.host)) {
                reply(json_error(403U, "Forbidden", ErrorCode::permission_denied,
                                 "GUI Host header does not match the loopback listener"));
                return;
            }
            if (buffer_.size() > expected_body_bytes_) {
                reply(json_error(400U, "Bad Request", ErrorCode::invalid_argument,
                                 "HTTP pipelining is unsupported"));
                return;
            }
            if (buffer_.size() == expected_body_bytes_) {
                consume_body();
                return;
            }
            auto self = shared_from_this();
            asio::async_read(
                socket_, buffer_, asio::transfer_exactly(expected_body_bytes_ - buffer_.size()),
                [self](const asio::error_code& body_error, const std::size_t) {
                    if (body_error) {
                        self->reply(json_error(400U, "Bad Request", ErrorCode::invalid_argument,
                                               "request body is incomplete"));
                        return;
                    }
                    self->consume_body();
                });
        }

        void consume_body() {
            request_.body.assign(expected_body_bytes_, '\0');
            std::istream input{&buffer_};
            input.read(request_.body.data(), static_cast<std::streamsize>(request_.body.size()));
            if (buffer_.size() != 0U) {
                reply(json_error(400U, "Bad Request", ErrorCode::invalid_argument,
                                 "HTTP pipelining is unsupported"));
                return;
            }
            const bool head = request_.method == "HEAD";
            if (request_.path.starts_with("/api/")) {
                auto self = shared_from_this();
                owner_->dispatch_api(std::move(request_), [self](HttpResponse response) mutable {
                    self->reply(std::move(response));
                });
                return;
            }
            if (request_.method != "GET" && !head) {
                reply(json_error(405U, "Method Not Allowed", ErrorCode::invalid_argument,
                                 "method not allowed"));
                return;
            }
            reply(owner_->static_response(request_.path), head);
        }

        void reply(HttpResponse response, const bool head = false) {
            if (finished_.load()) {
                return;
            }
            response_ = serialize_response(response, head);
            auto self = shared_from_this();
            asio::async_write(
                socket_, asio::buffer(response_),
                [self](const asio::error_code&, const std::size_t) { self->close(); });
        }

        void close() noexcept {
            if (finished_.exchange(true)) {
                return;
            }
            asio::error_code ignored;
            try {
                static_cast<void>(timer_.cancel());
            } catch (...) {
            }
            socket_.cancel(ignored);
            socket_.shutdown(tcp::socket::shutdown_both, ignored);
            socket_.close(ignored);
            owner_->active_connections_.fetch_sub(1U);
        }

        std::shared_ptr<Impl> owner_;
        tcp::socket socket_;
        asio::steady_timer timer_;
        asio::streambuf buffer_;
        HttpRequest request_;
        std::size_t expected_body_bytes_{0U};
        std::string response_;
        std::atomic_bool finished_{false};
    };

  public:
    Impl(asio::io_context& io_context, tcp::endpoint endpoint, ServerOptions options,
         std::filesystem::path assets_directory)
        : io_context_(io_context), acceptor_(io_context), endpoint_(std::move(endpoint)),
          options_(std::move(options)), assets_directory_(std::move(assets_directory)),
          workers_(options_.worker_threads) {}

    ~Impl() noexcept { stop(); }

    [[nodiscard]] Result<void> start() {
        if (running_.exchange(true)) {
            return Error{ErrorCode::already_exists, "GUI listener is already running"};
        }
        asio::error_code error;
        acceptor_.open(endpoint_.protocol(), error);
        if (!error) {
            acceptor_.set_option(tcp::acceptor::reuse_address{true}, error);
        }
        if (!error) {
            acceptor_.bind(endpoint_, error);
        }
        if (!error) {
            acceptor_.listen(
                static_cast<int>(std::min<std::size_t>(options_.max_connections, 128U)), error);
        }
        if (error) {
            running_.store(false);
            return Error{error == asio::error::address_in_use ? ErrorCode::remote_port_in_use
                                                              : ErrorCode::connection_failed,
                         "GUI listener could not bind"};
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
        workers_.stop();
        workers_.join();
    }

    [[nodiscard]] std::uint16_t listening_port() const noexcept {
        asio::error_code error;
        const auto endpoint = acceptor_.local_endpoint(error);
        return error ? 0U : endpoint.port();
    }

  private:
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
            if (self->running_.load()) {
                self->accept_next();
            }
        });
    }

    [[nodiscard]] HttpResponse static_response(const std::string_view request_path) const {
        if (!valid_asset_path(request_path)) {
            return json_error(400U, "Bad Request", ErrorCode::invalid_argument,
                              "asset path is invalid");
        }
        const std::string relative =
            request_path == "/" ? "index.html" : std::string{request_path.substr(1U)};
        std::error_code filesystem_error;
        const std::filesystem::path path =
            std::filesystem::weakly_canonical(assets_directory_ / relative, filesystem_error);
        if (filesystem_error || !path_is_within(assets_directory_, path)) {
            return json_error(404U, "Not Found", ErrorCode::not_found, "GUI asset was not found");
        }
        auto body = read_asset(path, options_.max_asset_bytes);
        if (!body) {
            return json_error(body.error().code() == ErrorCode::not_found ? 404U : 413U,
                              body.error().code() == ErrorCode::not_found ? "Not Found"
                                                                          : "Content Too Large",
                              body.error().code(), body.error().message());
        }
        const bool immutable = request_path.starts_with("/assets/");
        return {200U, "OK", content_type_for(path), std::move(*body),
                immutable ? "public, max-age=31536000, immutable" : "no-cache"};
    }

    void dispatch_api(HttpRequest request, std::function<void(HttpResponse)> completion) {
        auto self = shared_from_this();
        asio::post(workers_, [self, request = std::move(request),
                              completion = std::move(completion)]() mutable {
            HttpResponse response = self->api_response(request);
            asio::post(self->io_context_, [completion = std::move(completion),
                                           response = std::move(response)]() mutable {
                completion(std::move(response));
            });
        });
    }

    [[nodiscard]] HttpResponse api_response(const HttpRequest& request) const {
        if (!request.path.starts_with("/api/v1/")) {
            return json_error(404U, "Not Found", ErrorCode::not_found,
                              "API endpoint was not found");
        }
        const bool mutation = request.method != "GET" && request.method != "HEAD";
        if (mutation &&
            (!request.origin.has_value() || *request.origin != "http://" + request.host ||
             !is_json_content_type(request.content_type))) {
            return json_error(403U, "Forbidden", ErrorCode::permission_denied,
                              "GUI mutation requires same-origin JSON");
        }

        Json params = Json::object();
        if (mutation) {
            try {
                params = Json::parse(request.body);
            } catch (...) {
                return json_error(400U, "Bad Request", ErrorCode::invalid_argument,
                                  "request body must be valid JSON");
            }
            if (!params.is_object()) {
                return json_error(400U, "Bad Request", ErrorCode::invalid_argument,
                                  "request body must be a JSON object");
            }
        } else if (!request.body.empty()) {
            return json_error(400U, "Bad Request", ErrorCode::invalid_argument,
                              "GET request body is not allowed");
        }

        const auto segments = path_segments(request.path);
        std::string method;
        if (request.method == "GET" &&
            segments == std::vector<std::string>{"api", "v1", "status"}) {
            method = "status";
        } else if (request.method == "GET" &&
                   segments == std::vector<std::string>{"api", "v1", "servers"}) {
            method = "server.list";
        } else if (request.method == "GET" &&
                   segments == std::vector<std::string>{"api", "v1", "tunnels"}) {
            method = "tun.list";
        } else if (request.method == "GET" &&
                   segments == std::vector<std::string>{"api", "v1", "diagnostics"}) {
            method = "doctor";
        } else if (request.method == "POST" &&
                   segments == std::vector<std::string>{"api", "v1", "tunnels"}) {
            method = "tun.add";
        } else if (request.method == "POST" && segments.size() == 5U && segments[0] == "api" &&
                   segments[1] == "v1" && segments[2] == "tunnels" &&
                   (segments[4] == "enable" || segments[4] == "disable" ||
                    segments[4] == "remove")) {
            method = "tun." + segments[4];
            params = Json{{"identifier", segments[3]}};
        } else {
            return json_error(404U, "Not Found", ErrorCode::not_found,
                              "API endpoint was not found");
        }

        auto request_id = common::Id::generate(common::IdKind::request);
        if (!request_id) {
            return json_error(500U, "Internal Server Error", request_id.error().code(),
                              request_id.error().message());
        }
        ipc::LocalClient client{
            {.socket_path = options_.socket_path,
             .connect_timeout = std::chrono::milliseconds{2'000},
             .request_timeout =
                 std::chrono::duration_cast<std::chrono::milliseconds>(options_.request_timeout)}};
        auto response = client.request(
            {ipc::kProtocolVersion, std::move(*request_id), method, std::move(params)});
        if (!response) {
            return json_error(502U, "Bad Gateway", response.error().code(),
                              response.error().message());
        }
        if (!response->ok()) {
            const Error& error = *response->error();
            const unsigned int status = error.code() == ErrorCode::not_found           ? 404U
                                        : error.code() == ErrorCode::permission_denied ? 403U
                                        : error.code() == ErrorCode::already_exists    ? 409U
                                                                                       : 400U;
            return json_error(status,
                              status == 404U   ? "Not Found"
                              : status == 403U ? "Forbidden"
                              : status == 409U ? "Conflict"
                                               : "Bad Request",
                              error.code(), error.message());
        }
        return {200U, "OK", "application/json; charset=utf-8", response->result()->dump(),
                "no-store"};
    }

    [[nodiscard]] bool accepts_host(const std::string_view authority) const {
        std::string endpoint_text{authority};
        if (endpoint_.port() == 80U) {
            if (authority.starts_with('[') && authority.ends_with(']')) {
                endpoint_text.append(":80");
            } else if (authority.find(':') == std::string_view::npos) {
                endpoint_text.append(":80");
            }
        }
        const auto parsed = common::Endpoint::parse(endpoint_text);
        if (!parsed || parsed->is_domain() || parsed->port() != endpoint_.port()) {
            return false;
        }
        asio::error_code error;
        const auto address = asio::ip::make_address(parsed->host(), error);
        return !error && address == endpoint_.address();
    }

    asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    tcp::endpoint endpoint_;
    ServerOptions options_;
    std::filesystem::path assets_directory_;
    asio::thread_pool workers_;
    std::atomic<std::size_t> active_connections_{0U};
    std::atomic_bool running_{false};
};

Result<std::unique_ptr<Server>> Server::create(asio::io_context& io_context,
                                               ServerOptions options) {
    auto endpoint = common::Endpoint::parse(options.listen_endpoint);
    if (!endpoint) {
        return endpoint.error();
    }
    asio::error_code error;
    const auto address = asio::ip::make_address(endpoint->host(), error);
    if (error || !address.is_loopback()) {
        return Error{ErrorCode::permission_denied,
                     "GUI listener must use a numeric loopback address"};
    }
    if (options.assets_directory.empty() || options.socket_path.empty() ||
        options.max_connections == 0U || options.max_connections > 1'024U ||
        options.worker_threads == 0U || options.worker_threads > 32U ||
        options.max_header_bytes < 1'024U || options.max_header_bytes > 64U * 1024U ||
        options.max_body_bytes == 0U || options.max_body_bytes > 4U * 1024U * 1024U ||
        options.max_asset_bytes == 0U || options.max_asset_bytes > 16U * 1024U * 1024U ||
        options.request_timeout <= std::chrono::seconds::zero() ||
        options.request_timeout > std::chrono::minutes{5}) {
        return Error{ErrorCode::invalid_argument, "GUI server options are invalid"};
    }
    std::error_code filesystem_error;
    auto assets = std::filesystem::weakly_canonical(options.assets_directory, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(assets, filesystem_error)) {
        return Error{ErrorCode::not_found, "GUI assets directory is unavailable"};
    }
    try {
        auto implementation =
            std::make_shared<Impl>(io_context, tcp::endpoint{address, endpoint->port()},
                                   std::move(options), std::move(assets));
        return std::unique_ptr<Server>{new Server{std::move(implementation)}};
    } catch (...) {
        return Error{ErrorCode::resource_exhausted, "GUI server could not be allocated"};
    }
}

Server::Server(std::shared_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
Server::~Server() noexcept { stop(); }
Result<void> Server::start() { return implementation_->start(); }
void Server::stop() noexcept { implementation_->stop(); }
std::uint16_t Server::listening_port() const noexcept { return implementation_->listening_port(); }

} // namespace minitun::gui
