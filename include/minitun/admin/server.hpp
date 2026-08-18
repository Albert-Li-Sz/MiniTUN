#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>

#include <minitun/common/result.hpp>

namespace minitun::admin {

struct ServerOptions final {
    std::string listen_endpoint;
    std::string token_file{};
    std::size_t max_connections{64U};
    std::size_t max_header_bytes{8U * 1024U};
    std::size_t max_body_bytes{64U * 1024U};
    std::chrono::seconds timeout{5};
};

struct ManagementRequest final {
    std::string method;
    /// Absolute request path without a query component.
    std::string path;
    std::string body;
};

struct ManagementResponse final {
    unsigned int status{200U};
    std::string reason{"OK"};
    std::string content_type{"application/json"};
    std::string body;
};

struct Providers final {
    std::function<bool()> healthy;
    std::function<bool()> ready;
    std::function<std::string()> metrics;
    /// Optional management handler enabling the /v1/* endpoint surface. When
    /// set and a bearer token is configured, /v1/* requires authentication.
    std::function<common::Result<ManagementResponse>(const ManagementRequest&)> management;
};

/// The parsed shape of a single HTTP/1.1 management request (request line and
/// headers only). Exposed for the fuzz target and integration tests; the
/// authorization value itself is intentionally not returned.
struct ParsedHttpRequest final {
    std::string method;
    std::string path;
    bool has_authorization{false};
    std::size_t content_length{0U};
};

/// Parses a raw HTTP/1.1 request text (headers only, terminated by CRLF CRLF).
/// Returns an error for malformed framing, request line, or headers.
[[nodiscard]] common::Result<ParsedHttpRequest> parse_http_request(std::string_view text);

/// A deliberately small HTTP/1.1 management listener. Every response closes
/// the connection; request bodies, transfer encoding, pipelining, and methods
/// outside the documented endpoint surface are rejected.
class Server final {
  public:
    [[nodiscard]] static common::Result<std::unique_ptr<Server>>
    create(asio::io_context& io_context, ServerOptions options, Providers providers);

    ~Server() noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] common::Result<void> start();
    void stop() noexcept;
    [[nodiscard]] std::uint16_t listening_port() const noexcept;

  private:
    class Impl;
    explicit Server(std::shared_ptr<Impl> implementation) noexcept;
    std::shared_ptr<Impl> implementation_;
};

} // namespace minitun::admin
