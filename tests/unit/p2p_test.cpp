#include <chrono>
#include <exception>
#include <optional>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/p2p.hpp>

namespace minitun::protocol {
namespace {

TEST(P2pTest, DefinesStablePathAndZeroStatistics) {
    EXPECT_NE(P2pPath::direct, P2pPath::relay);
    EXPECT_EQ(P2pRelayStats{}, P2pRelayStats{});
}

TEST(P2pTest, RejectsInvalidUpgradeAndRelayInputs) {
    asio::io_context io_context;
    asio::ssl::context tls_context{asio::ssl::context::tls_client};
    TlsStream tls_stream{io_context, tls_context};
    asio::ip::tcp::socket first{io_context};
    asio::ip::tcp::socket second{io_context};
    std::optional<common::Result<P2pHostUpgrade>> host;
    std::optional<common::Result<P2pPeerUpgrade>> peer;
    std::optional<common::Result<P2pRelayStats>> relay;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            host = co_await accept_p2p_upgrade(tls_stream, asio::ip::address_v4{},
                                               std::chrono::seconds{1});
            peer = co_await connect_p2p_upgrade(asio::ip::tcp::socket{io_context},
                                                std::chrono::seconds{1}, std::chrono::seconds{1});
            relay = co_await relay_tcp_and_tcp(first, second, std::chrono::seconds{1});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(host.has_value());
    ASSERT_TRUE(peer.has_value());
    ASSERT_TRUE(relay.has_value());
    EXPECT_FALSE(*host);
    EXPECT_FALSE(*peer);
    EXPECT_FALSE(*relay);
    EXPECT_EQ(host->error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(peer->error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(relay->error().code(), common::ErrorCode::invalid_argument);
}

} // namespace
} // namespace minitun::protocol
