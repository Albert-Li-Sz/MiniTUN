#include <cstdint>
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

} // namespace
} // namespace minitun::server
