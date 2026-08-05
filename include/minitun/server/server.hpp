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
    std::size_t max_total_tunnels{10'000U};
    std::size_t max_connections_per_client{10'000U};
    std::size_t max_total_connections{50'000U};
    std::chrono::seconds handshake_timeout{10};
    std::chrono::seconds heartbeat_interval{5};
    std::chrono::seconds heartbeat_timeout{15};
    std::chrono::seconds allowed_clock_skew{30};
    std::chrono::seconds worker_wait_timeout{2};
    std::chrono::seconds worker_idle_timeout{60};
    std::chrono::seconds relay_inactivity_timeout{300};
    std::chrono::seconds graceful_shutdown_timeout{10};

    std::uint16_t min_idle_workers{2U};
    std::uint16_t max_idle_workers{32U};
    std::size_t max_total_idle_workers{128U};
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
    [[nodiscard]] common::Result<void> reload();
    void stop() noexcept;

    [[nodiscard]] std::uint16_t listening_port() const noexcept;
    [[nodiscard]] const std::string& server_id() const noexcept;

  private:
    class Impl;

    explicit Server(std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace minitun::server
