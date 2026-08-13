#include <chrono>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl/context.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/p2p.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::pair<asio::ip::tcp::socket, asio::ip::tcp::socket>
connected_pair(asio::io_context& io_context) {
    asio::ip::tcp::acceptor acceptor{io_context, {asio::ip::tcp::v4(), 0U}};
    asio::ip::tcp::socket client{io_context};
    asio::ip::tcp::socket server{io_context};
    client.connect(acceptor.local_endpoint());
    acceptor.accept(server);
    return {std::move(client), std::move(server)};
}

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

TEST(P2pTest, RejectsInvalidRelayTimeouts) {
    asio::io_context io_context;
    auto first = connected_pair(io_context);
    auto second = connected_pair(io_context);
    std::optional<common::Result<P2pRelayStats>> zero;
    std::optional<common::Result<P2pRelayStats>> oversized;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            zero = co_await relay_tcp_and_tcp(first.second, second.second,
                                              std::chrono::seconds{0});
            oversized = co_await relay_tcp_and_tcp(first.second, second.second,
                                                   std::chrono::hours{25});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(zero.has_value());
    ASSERT_TRUE(oversized.has_value());
    ASSERT_FALSE(*zero);
    ASSERT_FALSE(*oversized);
    EXPECT_EQ(zero->error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(oversized->error().code(), common::ErrorCode::invalid_argument);
}

TEST(P2pTest, RelaysBytesInBothDirectionsAndCompletesOnEof) {
    asio::io_context io_context;
    auto first = connected_pair(io_context);
    auto second = connected_pair(io_context);
    std::optional<common::Result<P2pRelayStats>> relay_result;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            relay_result = co_await relay_tcp_and_tcp(first.second, second.second,
                                                      std::chrono::seconds{5});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            const std::string first_to_second{"ping"};
            const std::string second_to_first{"pong"};
            co_await asio::async_write(first.first, asio::buffer(first_to_second),
                                       asio::use_awaitable);
            std::array<char, 4U> received{};
            co_await asio::async_read(second.first, asio::buffer(received),
                                      asio::use_awaitable);
            const std::string_view first_reply{received.data(), received.size()};
            EXPECT_EQ(first_reply, "ping");
            co_await asio::async_write(second.first, asio::buffer(second_to_first),
                                       asio::use_awaitable);
            co_await asio::async_read(first.first, asio::buffer(received),
                                      asio::use_awaitable);
            const std::string_view second_reply{received.data(), received.size()};
            EXPECT_EQ(second_reply, "pong");
            first.first.shutdown(asio::ip::tcp::socket::shutdown_send);
            second.first.shutdown(asio::ip::tcp::socket::shutdown_send);
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(relay_result.has_value());
    ASSERT_TRUE(*relay_result) << relay_result->error();
    EXPECT_EQ((*relay_result)->first_to_second_bytes, 4U);
    EXPECT_EQ((*relay_result)->second_to_first_bytes, 4U);
}

TEST(P2pTest, RelayFailsWhenIdleBeyondInactivityTimeout) {
    asio::io_context io_context;
    auto first = connected_pair(io_context);
    auto second = connected_pair(io_context);
    std::optional<common::Result<P2pRelayStats>> relay_result;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            relay_result = co_await relay_tcp_and_tcp(first.second, second.second,
                                                      std::chrono::seconds{1});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(relay_result.has_value());
    ASSERT_FALSE(*relay_result);
    EXPECT_EQ(relay_result->error().code(), common::ErrorCode::connection_timeout);
}

TEST(P2pTest, RelayCompletesWhenPeerResetsConnection) {
    asio::io_context io_context;
    auto first = connected_pair(io_context);
    auto second = connected_pair(io_context);
    std::optional<common::Result<P2pRelayStats>> relay_result;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            relay_result = co_await relay_tcp_and_tcp(first.second, second.second,
                                                      std::chrono::seconds{5});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            const std::array<std::uint8_t, 1U> byte{0x42U};
            co_await asio::async_write(first.first, asio::buffer(byte), asio::use_awaitable);
            first.first.set_option(asio::socket_base::linger{true, 0});
            first.first.close();
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(relay_result.has_value());
    ASSERT_TRUE(*relay_result) << relay_result->error();
    EXPECT_EQ((*relay_result)->first_to_second_bytes, 1U);
}

TEST(P2pTest, ConfirmsDirectPathWithReadyMagic) {
    asio::io_context io_context;
    auto pair = connected_pair(io_context);
    std::optional<common::Result<void>> confirmed;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            confirmed = co_await confirm_p2p_direct(pair.second);
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            const std::array<std::uint8_t, 4U> ready{'M', 'T', 'O', 'K'};
            co_await asio::async_write(pair.first, asio::buffer(ready), asio::use_awaitable);
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(confirmed.has_value());
    ASSERT_TRUE(*confirmed) << confirmed->error();

    io_context.restart();
    asio::ip::tcp::socket closed{io_context};
    std::optional<common::Result<void>> rejected;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            rejected = co_await confirm_p2p_direct(closed);
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(rejected.has_value());
    ASSERT_FALSE(*rejected);
    EXPECT_EQ(rejected->error().code(), common::ErrorCode::connection_failed);
}

} // namespace
} // namespace minitun::protocol
