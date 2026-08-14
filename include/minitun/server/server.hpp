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
    std::string clients_config_path;
    std::string client_ca_path;
    // Internal listener cap. Public CLI policy is configured per client.
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
    std::size_t max_udp_peer_sessions{128U};
};

/// Process-local counters reset whenever minitun-server restarts.  The
/// management endpoint deliberately renders these without client or tunnel
/// labels so cardinality remains bounded.
struct ServerMetrics final {
    std::uint64_t active_sessions{0U};
    std::uint64_t active_connections{0U};
    std::uint64_t active_tunnels{0U};
    std::uint64_t idle_workers{0U};
    std::uint64_t active_relays{0U};
    std::uint64_t pending_connections{0U};
    std::uint64_t connections_total{0U};
    std::uint64_t tls_resumptions_total{0U};
    std::uint64_t authentication_success_total{0U};
    std::uint64_t authentication_failure_total{0U};
    std::uint64_t registration_success_total{0U};
    std::uint64_t registration_failure_total{0U};
    std::uint64_t unregistration_total{0U};
    std::uint64_t relay_total{0U};
    std::uint64_t relay_bytes_in_total{0U};
    std::uint64_t relay_bytes_out_total{0U};
    std::uint64_t acl_rejections_total{0U};
    std::uint64_t quota_rejections_total{0U};
    std::uint64_t source_rejections_total{0U};
    std::uint64_t errors_total{0U};
    std::uint64_t policy_reloads_total{0U};
    std::uint64_t policy_reload_failures_total{0U};
    std::uint64_t registration_latency_microseconds_total{0U};
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
    [[nodiscard]] ServerMetrics metrics() const noexcept;

  private:
    class Impl;

    explicit Server(std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace minitun::server
