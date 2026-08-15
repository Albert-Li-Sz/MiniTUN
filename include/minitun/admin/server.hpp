#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
