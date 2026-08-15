#include <chrono>
#include <exception>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/context.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/auth.hpp>
#include <minitun/protocol/p2p.hpp>

namespace minitun::protocol {
namespace {

template <typename Stream>
[[nodiscard]] asio::awaitable<bool> raw_write_exact(Stream& stream, const void* data,
                                                    const std::size_t size) {
    asio::error_code error;
    const std::size_t written = co_await asio::async_write(
        stream, asio::buffer(data, size), asio::redirect_error(asio::use_awaitable, error));
    co_return !error && written == size;
}

template <typename Stream>
[[nodiscard]] asio::awaitable<bool> raw_read_exact(Stream& stream, void* data,
                                                   const std::size_t size) {
    asio::error_code error;
    const std::size_t read = co_await asio::async_read(
        stream, asio::buffer(data, size), asio::redirect_error(asio::use_awaitable, error));
    co_return !error && read == size;
}

[[nodiscard]] std::vector<std::uint8_t> offer_bytes(const std::uint16_t port,
                                                    const AuthenticationNonce& token) {
    std::vector<std::uint8_t> offer{'M', 'T', 'P', '2', 1U, 4U,
                                    static_cast<std::uint8_t>((port >> 8U) & 0xffU),
                                    static_cast<std::uint8_t>(port & 0xffU)};
    const auto address = asio::ip::address_v4::loopback().to_bytes();
    offer.insert(offer.end(), address.begin(), address.end());
    offer.insert(offer.end(), token.begin(), token.end());
    return offer;
}

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

TEST(P2pTest, ConfirmDirectPathRequiresAnUpgradedTlsStream) {
    asio::io_context io_context;
    auto pair = connected_pair(io_context);
    asio::ssl::context tls_context{asio::ssl::context::tlsv13_server};
    TlsStream stream{std::move(pair.second), tls_context};
    std::optional<common::Result<void>> rejected;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            // Writing the ready magic on a stream that never completed the
            // TLS 1.3 PSK handshake must fail instead of leaking plaintext.
            rejected = co_await confirm_p2p_direct(stream);
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(rejected.has_value());
    ASSERT_FALSE(*rejected);
    EXPECT_EQ(rejected->error().code(), common::ErrorCode::connection_failed);
}

[[nodiscard]] std::uint16_t available_loopback_port(asio::io_context& io_context) {
    asio::ip::tcp::acceptor probe{io_context,
                                  asio::ip::tcp::endpoint{asio::ip::tcp::v4(), 0U}};
    return probe.local_endpoint().port();
}

TEST(P2pTest, CreatesSimultaneousOpenSocketsFromTheListenerPort) {
    asio::io_context io_context;
    const auto port = available_loopback_port(io_context);
    asio::ip::tcp::acceptor listener{
        io_context, asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), port}};
    const auto listener_endpoint = listener.local_endpoint();

    auto shared = create_simultaneous_open_socket(
        io_context.get_executor(), listener_endpoint,
        asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), 43'210U});
    ASSERT_TRUE(shared) << shared.error();
    const auto local = (*shared)->local_endpoint();
    // Reusing the listener port is the preferred outcome; platforms that
    // refuse it degrade to an ephemeral port.
    EXPECT_TRUE(local.port() == listener_endpoint.port() || local.port() != 0U);
    EXPECT_EQ(local.address(), asio::ip::address_v4::loopback());
    asio::error_code ignored;
    (*shared)->close(ignored);

    auto mismatch = create_simultaneous_open_socket(
        io_context.get_executor(), listener_endpoint,
        asio::ip::tcp::endpoint{asio::ip::address_v6::loopback(), 43'210U});
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code(), common::ErrorCode::invalid_argument);

    auto zero_port = create_simultaneous_open_socket(
        io_context.get_executor(), listener_endpoint,
        asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), 0U});
    ASSERT_FALSE(zero_port);
    EXPECT_EQ(zero_port.error().code(), common::ErrorCode::invalid_argument);
}

TEST(P2pTest, SimultaneousOpenConnectsBothSidesWithoutAListener) {
    asio::io_context io_context;
    asio::ip::tcp::socket first{io_context};
    asio::ip::tcp::socket second{io_context};
    asio::error_code error;
    first.open(asio::ip::tcp::v4(), error);
    ASSERT_FALSE(error);
    second.open(asio::ip::tcp::v4(), error);
    ASSERT_FALSE(error);
    first.set_option(asio::socket_base::reuse_address{true}, error);
    ASSERT_FALSE(error);
    second.set_option(asio::socket_base::reuse_address{true}, error);
    ASSERT_FALSE(error);

    const auto first_port = available_loopback_port(io_context);
    auto second_port = available_loopback_port(io_context);
    while (second_port == first_port) {
        second_port = available_loopback_port(io_context);
    }
    first.bind({asio::ip::address_v4::loopback(), first_port}, error);
    ASSERT_FALSE(error) << error.message();
    second.bind({asio::ip::address_v4::loopback(), second_port}, error);
    ASSERT_FALSE(error) << error.message();

    std::optional<asio::error_code> first_error;
    std::optional<asio::error_code> second_error;
    first.async_connect({asio::ip::address_v4::loopback(), second_port},
                        [&first_error](const asio::error_code& connect_error) {
                            first_error = connect_error;
                        });
    second.async_connect({asio::ip::address_v4::loopback(), first_port},
                         [&second_error](const asio::error_code& connect_error) {
                             second_error = connect_error;
                         });
    io_context.run();
    ASSERT_TRUE(first_error.has_value());
    ASSERT_TRUE(second_error.has_value());
    // Both SYN-SENT sockets transition to ESTABLISHED: classic TCP
    // simultaneous open without any listener.
    EXPECT_FALSE(*first_error) << first_error->message();
    EXPECT_FALSE(*second_error) << second_error->message();
}

TEST(P2pTest, PeerRequestsSimultaneousOpenAndReusesTheMappingPort) {
    asio::io_context io_context;
    // The relay/bootstrap channel between the peer and the mock host.
    auto pair = connected_pair(io_context);
    asio::ip::tcp::socket& host_relay = pair.second;
    asio::ip::tcp::socket peer_relay = std::move(pair.first);

    // The mock host binds its direct candidate and later reuses that port for
    // the outbound half of the simultaneous open.
    const auto candidate_port = available_loopback_port(io_context);
    asio::ip::tcp::acceptor direct_acceptor{
        io_context, {asio::ip::address_v4::loopback(), candidate_port}};
    AuthenticationNonce token{};
    token.fill(0x5aU);

    std::optional<common::Result<P2pPeerUpgrade>> peer_result;
    std::string mock_report;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            // Craft and send the offer with the mock's bound candidate.
            const auto offer = offer_bytes(candidate_port, token);
            if (!co_await raw_write_exact(host_relay, offer.data(), offer.size())) {
                mock_report = "offer";
                co_return;
            }
            // Accept the peer's ordinary direct attempt, learn its mapping
            // port, and drop the connection so the direct path fails.
            asio::error_code error;
            asio::ip::tcp::socket direct{io_context};
            direct = co_await direct_acceptor.async_accept(
                asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                mock_report = "accept";
                co_return;
            }
            const auto peer_port = direct.remote_endpoint().port();
            direct.close();
            direct_acceptor.close();

            // The peer must request the simultaneous open next.
            std::array<std::uint8_t, 4U> control{};
            if (!co_await raw_read_exact(host_relay, control.data(), control.size())) {
                mock_report = "control";
                co_return;
            }
            if (control != std::array<std::uint8_t, 4U>{'M', 'T', 'P', 'S'}) {
                mock_report = std::string{"magic:"} +
                              std::string{reinterpret_cast<const char*>(control.data()), 4U};
                co_return;
            }
            // Outbound half: a socket bound to the candidate port connects to
            // the peer's reused mapping port while the peer connects back.
            asio::ip::tcp::socket outbound{io_context};
            outbound.open(asio::ip::tcp::v4(), error);
            outbound.set_option(asio::socket_base::reuse_address{true}, error);
            outbound.bind({asio::ip::address_v4::loopback(), candidate_port}, error);
            if (error) {
                mock_report = "bind";
                co_return;
            }
            asio::error_code connect_error;
            co_await outbound.async_connect(
                {asio::ip::address_v4::loopback(), peer_port},
                asio::redirect_error(asio::use_awaitable, connect_error));
            if (connect_error) {
                mock_report = "connect";
                co_return;
            }
            // The peer's SO socket carries the same MTPD + token handshake.
            constexpr std::size_t kHandshakeSize = 4U + 32U;
            std::array<std::uint8_t, kHandshakeSize> handshake{};
            if (!co_await raw_read_exact(outbound, handshake.data(), handshake.size())) {
                mock_report = "handshake";
                co_return;
            }

            constexpr std::array<std::uint8_t, 4U> kMockDirectMagic{'M', 'T', 'P', 'D'};
            const bool magic_ok = std::equal(kMockDirectMagic.begin(), kMockDirectMagic.end(),
                                             handshake.begin());
            const bool valid_token =
                magic_ok && std::equal(token.begin(), token.end(), handshake.begin() + 4);
            // The mock speaks no TLS; shutting down forces the peer onto the
            // relay fallback, which the mock then confirms.
            asio::error_code shutdown_error;
            outbound.shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
            outbound.close();
            std::array<std::uint8_t, 4U> fallback{};
            if (!co_await raw_read_exact(host_relay, fallback.data(), fallback.size())) {
                mock_report = "fallback";
                co_return;
            }
            const bool valid_fallback =
                fallback == std::array<std::uint8_t, 4U>{'M', 'T', 'F', 'B'};
            const std::array<std::uint8_t, 4U> ready_magic{'M', 'T', 'O', 'K'};
            static_cast<void>(
                co_await raw_write_exact(host_relay, ready_magic.data(), ready_magic.size()));
            mock_report = !magic_ok ? "hs-magic" : !valid_token ? "hs-token"
                          : !valid_fallback ? "fb-magic"
                                            : "ok";
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            peer_result = co_await connect_p2p_upgrade(std::move(peer_relay),
                                                       std::chrono::seconds{5},
                                                       std::chrono::seconds{2}, true, true);
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();

    EXPECT_EQ(mock_report, "ok");
    ASSERT_TRUE(peer_result.has_value());
    // The mock dropped the SO socket before TLS, so the peer ends on the
    // confirmed relay fallback with the bootstrap socket intact.
    ASSERT_TRUE(*peer_result) << peer_result->error();
    EXPECT_EQ((*peer_result)->path, P2pPath::relay);
    EXPECT_NE((*peer_result)->socket, nullptr);
}

} // namespace
} // namespace minitun::protocol
