#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/storage/models.hpp>

namespace minitun::storage {
namespace {

TEST(StorageModelsTest, RoundTripsEveryPersistedEnumSpelling) {
    constexpr std::array protocols{TunnelProtocol::tcp};
    for (const auto value : protocols) {
        const auto parsed = tunnel_protocol_from_string(to_string(value));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, value);
    }

    constexpr std::array server_desired{
        ServerDesiredState::enabled,
        ServerDesiredState::disabled,
        ServerDesiredState::removed,
    };
    for (const auto value : server_desired) {
        const auto parsed = server_desired_state_from_string(to_string(value));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, value);
    }

    constexpr std::array server_actual{
        ServerActualState::not_authenticated,
        ServerActualState::disconnected,
        ServerActualState::connecting,
        ServerActualState::tls_handshake,
        ServerActualState::authenticating,
        ServerActualState::online,
        ServerActualState::backoff,
        ServerActualState::disabled,
        ServerActualState::error,
    };
    for (const auto value : server_actual) {
        const auto parsed = server_actual_state_from_string(to_string(value));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, value);
    }

    constexpr std::array tunnel_desired{
        TunnelDesiredState::active,
        TunnelDesiredState::disabled,
        TunnelDesiredState::removed,
    };
    for (const auto value : tunnel_desired) {
        const auto parsed = tunnel_desired_state_from_string(to_string(value));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, value);
    }

    constexpr std::array tunnel_actual{
        TunnelActualState::pending, TunnelActualState::registering, TunnelActualState::active,
        TunnelActualState::failed,  TunnelActualState::removing,    TunnelActualState::disabled,
    };
    for (const auto value : tunnel_actual) {
        const auto parsed = tunnel_actual_state_from_string(to_string(value));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, value);
    }
}

TEST(StorageModelsTest, StrictlyRejectsUnknownPersistedValues) {
    const auto protocol = tunnel_protocol_from_string("udp");
    const auto server_desired = server_desired_state_from_string("ENABLED");
    const auto server_actual = server_actual_state_from_string("unknown");
    const auto tunnel_desired = tunnel_desired_state_from_string("");
    const auto tunnel_actual = tunnel_actual_state_from_string("online");

    EXPECT_FALSE(protocol);
    EXPECT_FALSE(server_desired);
    EXPECT_FALSE(server_actual);
    EXPECT_FALSE(tunnel_desired);
    EXPECT_FALSE(tunnel_actual);
    EXPECT_EQ(protocol.error().code(), common::ErrorCode::invalid_argument);

    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_protocol = static_cast<TunnelProtocol>(255);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_server_desired = static_cast<ServerDesiredState>(255);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_server_actual = static_cast<ServerActualState>(255);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_tunnel_desired = static_cast<TunnelDesiredState>(255);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto invalid_tunnel_actual = static_cast<TunnelActualState>(255);
    EXPECT_EQ(to_string(invalid_protocol), "unknown");
    EXPECT_EQ(to_string(invalid_server_desired), "unknown");
    EXPECT_EQ(to_string(invalid_server_actual), "unknown");
    EXPECT_EQ(to_string(invalid_tunnel_desired), "unknown");
    EXPECT_EQ(to_string(invalid_tunnel_actual), "unknown");
}

TEST(StorageModelsTest, DefinesFiniteDefaultStorageLimits) {
    constexpr StorageLimits defaults;

    EXPECT_GT(defaults.max_servers, 0U);
    EXPECT_GT(defaults.max_tunnels, defaults.max_servers);
    EXPECT_EQ(defaults.max_servers, kDefaultMaxServers);
    EXPECT_EQ(defaults.max_tunnels, kDefaultMaxTunnels);
}

} // namespace
} // namespace minitun::storage
