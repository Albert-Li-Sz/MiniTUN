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
    std::chrono::seconds timeout{5};
};

struct Providers final {
    std::function<bool()> healthy;
    std::function<bool()> ready;
    std::function<std::string()> metrics;
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
