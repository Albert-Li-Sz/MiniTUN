#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <asio/io_context.hpp>

#include <minitun/common/result.hpp>

namespace minitun::server {

struct ServerOptions final {
    std::string listen_endpoint{"0.0.0.0:2333"};
    std::string tls_certificate_path;
    std::string tls_private_key_path;
    std::string token_file_path;
    std::string allowed_ports{"1-65535"};

    std::size_t max_clients{1'000U};
    std::size_t max_tunnels_per_client{128U};
    std::chrono::seconds handshake_timeout{10};
    std::chrono::seconds heartbeat_interval{5};
    std::chrono::seconds heartbeat_timeout{15};
    std::chrono::seconds allowed_clock_skew{30};

    std::uint16_t min_idle_workers{2U};
    std::uint16_t max_idle_workers{32U};
};

class Server final {
  public:
    [[nodiscard]] static common::Result<std::unique_ptr<Server>>
    create(asio::io_context& io_context, ServerOptions options);

    ~Server() noexcept;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    [[nodiscard]] common::Result<void> start();
    void stop() noexcept;

    [[nodiscard]] std::uint16_t listening_port() const noexcept;
    [[nodiscard]] const std::string& server_id() const noexcept;

  private:
    class Impl;

    explicit Server(std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace minitun::server
