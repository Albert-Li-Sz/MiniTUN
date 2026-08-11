#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <asio/io_context.hpp>

#include <minitun/common/result.hpp>

namespace minitun::gui {

struct ServerOptions final {
    std::string listen_endpoint{"127.0.0.1:6500"};
    std::string socket_path{"/run/minitun/minitun.sock"};
    std::string assets_directory;
    std::size_t max_connections{64U};
    std::size_t worker_threads{4U};
    std::size_t max_header_bytes{16U * 1024U};
    std::size_t max_body_bytes{1U * 1024U * 1024U};
    std::size_t max_asset_bytes{4U * 1024U * 1024U};
    std::chrono::seconds request_timeout{15};
};

/// Localhost-only HTTP gateway for the MiniTun GUI. Browser requests are
/// translated into the same bounded local IPC methods used by the CLI and SDK.
class Server final {
  public:
    [[nodiscard]] static common::Result<std::unique_ptr<Server>>
    create(asio::io_context& io_context, ServerOptions options);

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

} // namespace minitun::gui
