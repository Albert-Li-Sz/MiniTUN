#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/port_range.hpp>
#include <minitun/server/server.hpp>
#include <minitun/server/tunnel_registry.hpp>

namespace minitun::server {
namespace {

[[nodiscard]] std::string generated_id(const common::IdKind kind) {
    auto id = common::Id::generate(kind);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

[[nodiscard]] std::uint16_t available_port(asio::io_context& io_context) {
    asio::ip::tcp::acceptor probe{io_context, asio::ip::tcp::endpoint{asio::ip::tcp::v4(), 0U}};
    return probe.local_endpoint().port();
}

TEST(ServerOptionsTest, AllowsEveryValidTcpPortByDefault) {
    const ServerOptions options;
    EXPECT_EQ(options.allowed_ports, "1-65535");
    EXPECT_EQ(options.max_total_tunnels, 10'000U);

    const auto allowed = common::PortRange::parse(options.allowed_ports);
    ASSERT_TRUE(allowed) << allowed.error();
    EXPECT_TRUE(allowed->contains(1U));
    EXPECT_TRUE(allowed->contains(65'535U));
}

TEST(ServerOptionsTest, RejectsEveryIndependentResourceAndTimeoutLimit) {
    asio::io_context io_context;
    const auto expect_invalid = [&io_context](ServerOptions options) {
        const auto created = Server::create(io_context, std::move(options));
        ASSERT_FALSE(created);
        EXPECT_EQ(created.error().code(), common::ErrorCode::invalid_argument) << created.error();
    };

    {
        ServerOptions options;
        options.max_clients = 0U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_clients = 100'001U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_tunnels_per_client = 0U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_tunnels_per_client = 4'097U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_total_tunnels = 0U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_total_tunnels = 100'001U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_connections_per_client = 0U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_connections_per_client = 100'001U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_total_connections = 0U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_total_connections = 100'001U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_connections_per_client = 11U;
        options.max_total_connections = 10U;
        expect_invalid(std::move(options));
    }

    const auto expect_bad_timeout = [&expect_invalid](auto member,
                                                      const std::chrono::seconds value) {
        ServerOptions options;
        options.*member = value;
        expect_invalid(std::move(options));
    };
    expect_bad_timeout(&ServerOptions::handshake_timeout, std::chrono::seconds::zero());
    expect_bad_timeout(&ServerOptions::handshake_timeout, std::chrono::seconds{301});
    expect_bad_timeout(&ServerOptions::heartbeat_interval, std::chrono::seconds::zero());
    expect_bad_timeout(&ServerOptions::heartbeat_interval, std::chrono::seconds{301});
    expect_bad_timeout(&ServerOptions::heartbeat_timeout, std::chrono::seconds{5});
    expect_bad_timeout(&ServerOptions::heartbeat_timeout, std::chrono::seconds{301});
    expect_bad_timeout(&ServerOptions::allowed_clock_skew, std::chrono::seconds{-1});
    expect_bad_timeout(&ServerOptions::allowed_clock_skew, std::chrono::seconds{301});
    expect_bad_timeout(&ServerOptions::worker_wait_timeout, std::chrono::seconds::zero());
    expect_bad_timeout(&ServerOptions::worker_wait_timeout, std::chrono::seconds{301});
    expect_bad_timeout(&ServerOptions::worker_idle_timeout, std::chrono::seconds::zero());
    expect_bad_timeout(&ServerOptions::worker_idle_timeout, std::chrono::seconds{301});
    expect_bad_timeout(&ServerOptions::relay_inactivity_timeout, std::chrono::seconds::zero());
    expect_bad_timeout(&ServerOptions::relay_inactivity_timeout,
                       std::chrono::hours{24} + std::chrono::seconds{1});
    expect_bad_timeout(&ServerOptions::graceful_shutdown_timeout, std::chrono::seconds::zero());
    expect_bad_timeout(&ServerOptions::graceful_shutdown_timeout, std::chrono::seconds{301});

    {
        ServerOptions options;
        options.min_idle_workers = 33U;
        options.max_idle_workers = 32U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_idle_workers = 129U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_total_idle_workers = 0U;
        expect_invalid(std::move(options));
    }
    {
        ServerOptions options;
        options.max_total_idle_workers = 4'097U;
        expect_invalid(std::move(options));
    }
}

TEST(ServerOptionsTest, RejectsMalformedEndpointAllowlistAndTlsInputs) {
    asio::io_context io_context;
    const auto expect_invalid = [&io_context](ServerOptions options) {
        const auto created = Server::create(io_context, std::move(options));
        ASSERT_FALSE(created);
        EXPECT_EQ(created.error().code(), common::ErrorCode::invalid_argument) << created.error();
    };

    ServerOptions endpoint;
    endpoint.listen_endpoint = "missing-port";
    expect_invalid(std::move(endpoint));
    ServerOptions host;
    host.listen_endpoint = "localhost:2333";
    expect_invalid(std::move(host));
    ServerOptions ports;
    ports.allowed_ports = "udp";
    expect_invalid(std::move(ports));

    ServerOptions tls;
    tls.listen_endpoint = "127.0.0.1:0";
    const auto missing_tls = Server::create(io_context, std::move(tls));
    ASSERT_FALSE(missing_tls);
    EXPECT_EQ(missing_tls.error().code(), common::ErrorCode::invalid_argument);
}

TEST(TunnelRegistryTest, EnforcesGlobalTunnelLimitAcrossClients) {
    asio::io_context io_context;
    const std::uint16_t first_port = available_port(io_context);
    const std::uint16_t second_port = available_port(io_context);
    auto allowed = common::PortRange::parse("1-65535");
    ASSERT_TRUE(allowed) << allowed.error();
    TunnelRegistry registry{io_context.get_executor(), std::move(*allowed), 4U, 1U};

    const std::string first_client = generated_id(common::IdKind::client);
    const std::string second_client = generated_id(common::IdKind::client);
    const std::string first_tunnel = generated_id(common::IdKind::tunnel);
    const std::string second_tunnel = generated_id(common::IdKind::tunnel);
    ASSERT_TRUE(
        registry.register_tunnel({first_client, 1U, first_tunnel, "127.0.0.1", first_port}));

    const auto rejected =
        registry.register_tunnel({second_client, 1U, second_tunnel, "127.0.0.1", second_port});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), common::ErrorCode::resource_exhausted);
    registry.remove_client(first_client);
    EXPECT_TRUE(
        registry.register_tunnel({second_client, 1U, second_tunnel, "127.0.0.1", second_port}));
}

TEST(TunnelRegistryTest, EnforcesAllowlistOwnershipLimitsAndIdempotentRemoval) {
    asio::io_context io_context;
    const std::uint16_t port = available_port(io_context);
    auto allowed = common::PortRange::parse(std::to_string(port));
    ASSERT_TRUE(allowed) << allowed.error();
    TunnelRegistry registry{io_context.get_executor(), std::move(*allowed), 1U};

    const std::string first_client = generated_id(common::IdKind::client);
    const std::string second_client = generated_id(common::IdKind::client);
    const std::string first_tunnel = generated_id(common::IdKind::tunnel);
    const std::string second_tunnel = generated_id(common::IdKind::tunnel);
    const TunnelBinding first{first_client, 11U, first_tunnel, "127.0.0.1", port};

    ASSERT_TRUE(registry.register_tunnel(first));
    EXPECT_TRUE(registry.register_tunnel(first));
    EXPECT_EQ(registry.size(), 1U);
    EXPECT_EQ(registry.client_size(first_client), 1U);

    const auto over_limit =
        registry.register_tunnel({first_client, 11U, second_tunnel, "127.0.0.1", port});
    ASSERT_FALSE(over_limit);
    EXPECT_EQ(over_limit.error().code(), common::ErrorCode::resource_exhausted);

    const auto conflict =
        registry.register_tunnel({second_client, 22U, second_tunnel, "127.0.0.1", port});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code(), common::ErrorCode::remote_port_in_use);

    registry.unregister_tunnel(first_client, 99U, first_tunnel);
    EXPECT_EQ(registry.size(), 1U);
    registry.unregister_tunnel(first_client, 11U, first_tunnel);
    registry.unregister_tunnel(first_client, 11U, first_tunnel);
    EXPECT_EQ(registry.size(), 0U);
    EXPECT_TRUE(registry.register_tunnel({second_client, 22U, second_tunnel, "127.0.0.1", port}));
    registry.remove_client(second_client);
    EXPECT_EQ(registry.size(), 0U);
}

TEST(TunnelRegistryTest, RejectsOutOfPolicyAndMalformedBindings) {
    asio::io_context io_context;
    auto allowed = common::PortRange::parse("6000-6001");
    ASSERT_TRUE(allowed);
    TunnelRegistry registry{io_context.get_executor(), std::move(*allowed), 2U};
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);

    const auto denied = registry.register_tunnel({client_id, 1U, tunnel_id, "127.0.0.1", 5'999U});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), common::ErrorCode::permission_denied);
    EXPECT_FALSE(registry.register_tunnel({client_id, 0U, tunnel_id, "127.0.0.1", 6'000U}));
    EXPECT_FALSE(registry.register_tunnel({client_id, 1U, tunnel_id, "localhost", 6'000U}));
}

TEST(TunnelRegistryTest, RejectsEveryIndependentBindingAndRegistryLimit) {
    asio::io_context io_context;
    const std::uint16_t port = available_port(io_context);
    auto allowed = common::PortRange::parse(std::to_string(port));
    ASSERT_TRUE(allowed) << allowed.error();
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const TunnelBinding valid{client_id, 1U, tunnel_id, "127.0.0.1", port, 1U};

    TunnelRegistry registry{io_context.get_executor(), *allowed, 2U};
    const std::string invalid_client = generated_id(common::IdKind::server);
    const std::string invalid_tunnel = generated_id(common::IdKind::server);
    EXPECT_FALSE(registry.register_tunnel({invalid_client, 1U, tunnel_id, "127.0.0.1", port, 1U}));
    EXPECT_FALSE(registry.register_tunnel({client_id, 1U, invalid_tunnel, "127.0.0.1", port, 1U}));
    EXPECT_FALSE(registry.register_tunnel({client_id, 0U, tunnel_id, "127.0.0.1", port, 1U}));
    EXPECT_FALSE(registry.register_tunnel({client_id, 1U, tunnel_id, "127.0.0.1", port, 0U}));
    EXPECT_FALSE(registry.register_tunnel({client_id, 1U, tunnel_id, "", port, 1U}));
    EXPECT_FALSE(registry.register_tunnel({client_id, 1U, tunnel_id, "127.0.0.1", 0U, 1U}));

    TunnelRegistry zero_client{io_context.get_executor(), *allowed, 0U};
    EXPECT_FALSE(zero_client.register_tunnel(valid));
    TunnelRegistry large_client{io_context.get_executor(), *allowed, 100'001U};
    EXPECT_FALSE(large_client.register_tunnel(valid));
    TunnelRegistry zero_global{io_context.get_executor(), *allowed, 2U, 0U};
    EXPECT_FALSE(zero_global.register_tunnel(valid));
    TunnelRegistry large_global{io_context.get_executor(), *allowed, 2U, 100'001U};
    EXPECT_FALSE(large_global.register_tunnel(valid));
}

TEST(TunnelRegistryTest, ReplacesChangedRevisionOnlyAfterWithdrawingOldListener) {
    asio::io_context io_context;
    const std::uint16_t first_port = available_port(io_context);
    auto allowed = common::PortRange::parse("1-65535");
    ASSERT_TRUE(allowed) << allowed.error();
    TunnelRegistry registry{io_context.get_executor(), std::move(*allowed), 2U};
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);

    ASSERT_TRUE(registry.register_tunnel({client_id, 7U, tunnel_id, "127.0.0.1", first_port, 1U}));
    ASSERT_TRUE(registry.register_tunnel({client_id, 7U, tunnel_id, "127.0.0.1", first_port, 2U}));
    EXPECT_EQ(registry.size(), 1U);

    asio::ip::tcp::acceptor blocker{
        io_context, asio::ip::tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0U}};
    const std::uint16_t blocked_port = blocker.local_endpoint().port();
    const auto failed_replacement =
        registry.register_tunnel({client_id, 7U, tunnel_id, "127.0.0.1", blocked_port, 3U});
    ASSERT_FALSE(failed_replacement);
    EXPECT_EQ(failed_replacement.error().code(), common::ErrorCode::remote_port_in_use);
    EXPECT_EQ(registry.size(), 0U);

    asio::ip::tcp::acceptor old_port_probe{
        io_context, asio::ip::tcp::endpoint{asio::ip::tcp::v4(), first_port}};
    EXPECT_TRUE(old_port_probe.is_open());
}

TEST(TunnelRegistryTest, RemovesOnlyMatchingSessionAndRevisionThenClearsRemainder) {
    asio::io_context io_context;
    auto allowed = common::PortRange::parse("1-65535");
    ASSERT_TRUE(allowed) << allowed.error();
    TunnelRegistry registry{io_context.get_executor(), std::move(*allowed), 4U};
    const std::string first_client = generated_id(common::IdKind::client);
    const std::string second_client = generated_id(common::IdKind::client);
    const std::string first_tunnel = generated_id(common::IdKind::tunnel);
    const std::string second_tunnel = generated_id(common::IdKind::tunnel);
    const std::string third_tunnel = generated_id(common::IdKind::tunnel);
    ASSERT_TRUE(registry.register_tunnel(
        {first_client, 1U, first_tunnel, "127.0.0.1", available_port(io_context), 2U}));
    ASSERT_TRUE(registry.register_tunnel(
        {first_client, 2U, second_tunnel, "127.0.0.1", available_port(io_context), 3U}));
    ASSERT_TRUE(registry.register_tunnel(
        {second_client, 1U, third_tunnel, "127.0.0.1", available_port(io_context), 1U}));

    registry.unregister_tunnel(first_client, 1U, first_tunnel, 1U);
    EXPECT_EQ(registry.size(), 3U);
    registry.unregister_tunnel(first_client, 1U, first_tunnel, 2U);
    EXPECT_EQ(registry.size(), 2U);
    registry.remove_session(first_client, 1U);
    EXPECT_EQ(registry.size(), 2U);
    registry.remove_session(first_client, 2U);
    EXPECT_EQ(registry.size(), 1U);
    registry.clear();
    EXPECT_EQ(registry.size(), 0U);
}

TEST(TunnelRegistryTest, DispatchesPublicConnectionsAndContainsHandlerExceptions) {
    asio::io_context io_context;
    auto allowed = common::PortRange::parse("1-65535");
    ASSERT_TRUE(allowed) << allowed.error();
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const std::uint16_t port = available_port(io_context);
    std::size_t calls = 0U;
    TunnelRegistry registry{io_context.get_executor(), std::move(*allowed), 2U,
                            [&calls](const TunnelBinding, asio::ip::tcp::socket socket) {
                                ++calls;
                                asio::error_code ignored;
                                socket.close(ignored);
                                throw std::runtime_error("contained handler failure");
                            }};
    ASSERT_TRUE(registry.register_tunnel({client_id, 1U, tunnel_id, "127.0.0.1", port, 1U}));

    asio::ip::tcp::socket public_socket{io_context};
    public_socket.connect(asio::ip::tcp::endpoint{asio::ip::make_address("127.0.0.1"), port});
    io_context.run_for(std::chrono::milliseconds{100});
    EXPECT_EQ(calls, 1U);
}

} // namespace
} // namespace minitun::server
