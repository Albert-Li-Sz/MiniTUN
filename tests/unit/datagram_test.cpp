#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/read.hpp>
#include <asio/ssl/context.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/datagram.hpp>

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

[[nodiscard]] std::pair<asio::ip::udp::socket, asio::ip::udp::socket>
connected_udp_pair(asio::io_context& io_context) {
    asio::ip::udp::socket first{io_context,
                                asio::ip::udp::endpoint{asio::ip::address_v4::loopback(), 0U}};
    asio::ip::udp::socket second{io_context,
                                 asio::ip::udp::endpoint{asio::ip::address_v4::loopback(), 0U}};
    const auto first_endpoint = first.local_endpoint();
    const auto second_endpoint = second.local_endpoint();
    first.connect(second_endpoint);
    second.connect(first_endpoint);
    return {std::move(first), std::move(second)};
}

[[nodiscard]] std::vector<std::uint8_t>
frame_datagram(const std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> record;
    record.reserve(kDatagramRecordHeaderSize + payload.size());
    record.push_back(static_cast<std::uint8_t>((payload.size() >> 8U) & 0xffU));
    record.push_back(static_cast<std::uint8_t>(payload.size() & 0xffU));
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

asio::awaitable<void> write_datagram_record(asio::ip::tcp::socket& socket,
                                            const std::span<const std::uint8_t> payload) {
    const auto record = frame_datagram(payload);
    co_await asio::async_write(socket, asio::buffer(record), asio::use_awaitable);
}

[[nodiscard]] asio::awaitable<std::vector<std::uint8_t>>
read_datagram_record(asio::ip::tcp::socket& socket) {
    std::array<std::uint8_t, kDatagramRecordHeaderSize> header{};
    co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
    const std::size_t length =
        (static_cast<std::size_t>(header[0]) << 8U) | static_cast<std::size_t>(header[1]);
    std::vector<std::uint8_t> payload(length);
    if (length != 0U) {
        co_await asio::async_read(socket, asio::buffer(payload), asio::use_awaitable);
    }
    co_return payload;
}

TEST(DatagramTest, EncodesBoundedPayloadsWithNetworkOrderLength) {
    const std::array<std::uint8_t, 4U> payload{0x00U, 0x7fU, 0x80U, 0xffU};
    const auto encoded = encode_datagram_record(payload);
    ASSERT_TRUE(encoded) << encoded.error();
    EXPECT_EQ(*encoded, (std::vector<std::uint8_t>{0x00U, 0x04U, 0x00U, 0x7fU, 0x80U, 0xffU}));

    const auto empty = encode_datagram_record({});
    ASSERT_TRUE(empty) << empty.error();
    EXPECT_EQ(*empty, (std::vector<std::uint8_t>{0U, 0U}));

    std::vector<std::uint8_t> maximum(kMaximumUdpPayloadSize, 0x5aU);
    const auto maximum_record = encode_datagram_record(maximum);
    ASSERT_TRUE(maximum_record) << maximum_record.error();
    EXPECT_EQ(maximum_record->size(), kMaximumUdpPayloadSize + kDatagramRecordHeaderSize);
    EXPECT_EQ((*maximum_record)[0], 0xffU);
    EXPECT_EQ((*maximum_record)[1], 0xe3U);

    maximum.push_back(0U);
    const auto oversized = encode_datagram_record(maximum);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code(), common::ErrorCode::invalid_argument);
}

TEST(DatagramTest, RejectsInvalidRelayInputsBeforeDoingIo) {
    asio::io_context io_context;
    asio::ssl::context tls_context{asio::ssl::context::tls_client};
    TlsStream stream{io_context, tls_context};
    asio::ip::udp::socket socket{io_context};
    std::optional<common::Result<DatagramRelayStats>> result;
    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            result = co_await relay_tls_and_udp(
                stream, socket, {.inactivity_timeout = std::chrono::seconds::zero()});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code(), common::ErrorCode::invalid_argument);
}

TEST(DatagramTest, RejectsInvalidTcpRelayInputs) {
    asio::io_context io_context;
    auto tcp_pair = connected_pair(io_context);
    auto udp_pair = connected_udp_pair(io_context);
    asio::ip::tcp::socket closed_tcp{io_context};
    asio::ip::udp::socket closed_udp{io_context};

    std::optional<common::Result<DatagramRelayStats>> closed_tcp_result;
    std::optional<common::Result<DatagramRelayStats>> closed_udp_result;
    std::optional<common::Result<DatagramRelayStats>> zero_timeout_result;
    std::optional<common::Result<DatagramRelayStats>> oversized_timeout_result;

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            closed_tcp_result = co_await relay_tcp_and_udp(
                closed_tcp, udp_pair.second, {.inactivity_timeout = std::chrono::seconds{5}});
            closed_udp_result = co_await relay_tcp_and_udp(
                tcp_pair.second, closed_udp, {.inactivity_timeout = std::chrono::seconds{5}});
            zero_timeout_result = co_await relay_tcp_and_udp(
                tcp_pair.second, udp_pair.second,
                {.inactivity_timeout = std::chrono::seconds::zero()});
            oversized_timeout_result = co_await relay_tcp_and_udp(
                tcp_pair.second, udp_pair.second,
                {.inactivity_timeout = std::chrono::hours{24} + std::chrono::seconds{1}});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });

    io_context.run();

    ASSERT_TRUE(closed_tcp_result.has_value());
    ASSERT_FALSE(*closed_tcp_result);
    EXPECT_EQ(closed_tcp_result->error().code(), common::ErrorCode::invalid_argument);

    ASSERT_TRUE(closed_udp_result.has_value());
    ASSERT_FALSE(*closed_udp_result);
    EXPECT_EQ(closed_udp_result->error().code(), common::ErrorCode::invalid_argument);

    ASSERT_TRUE(zero_timeout_result.has_value());
    ASSERT_FALSE(*zero_timeout_result);
    EXPECT_EQ(zero_timeout_result->error().code(), common::ErrorCode::invalid_argument);

    ASSERT_TRUE(oversized_timeout_result.has_value());
    ASSERT_FALSE(*oversized_timeout_result);
    EXPECT_EQ(oversized_timeout_result->error().code(), common::ErrorCode::invalid_argument);
}

TEST(DatagramTest, RelaysDatagramsBothDirectionsOverTcp) {
    asio::io_context io_context;
    auto tcp_pair = connected_pair(io_context);
    auto udp_pair = connected_udp_pair(io_context);
    std::optional<common::Result<DatagramRelayStats>> relay_result;

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            relay_result = co_await relay_tcp_and_udp(
                tcp_pair.second, udp_pair.second,
                {.inactivity_timeout = std::chrono::seconds{5}});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            const std::array<std::uint8_t, 4U> first_payload{0xdeU, 0xadU, 0xbeU, 0xefU};
            const std::array<std::uint8_t, 3U> second_payload{1U, 2U, 3U};
            co_await write_datagram_record(tcp_pair.first, first_payload);
            co_await write_datagram_record(tcp_pair.first, second_payload);

            std::array<std::uint8_t, kMaximumUdpPayloadSize> received{};
            const std::size_t first_len =
                co_await udp_pair.first.async_receive(asio::buffer(received), asio::use_awaitable);
            EXPECT_EQ(first_len, first_payload.size());
            EXPECT_TRUE(std::equal(first_payload.begin(), first_payload.end(), received.begin()));
            const std::size_t second_len =
                co_await udp_pair.first.async_receive(asio::buffer(received), asio::use_awaitable);
            EXPECT_EQ(second_len, second_payload.size());
            EXPECT_TRUE(
                std::equal(second_payload.begin(), second_payload.end(), received.begin()));

            const std::array<std::uint8_t, 5U> third_payload{0x68U, 0x65U, 0x6cU, 0x6cU, 0x6fU};
            const std::size_t sent = co_await udp_pair.first.async_send(
                asio::buffer(third_payload), asio::use_awaitable);
            EXPECT_EQ(sent, third_payload.size());
            const auto echoed = co_await read_datagram_record(tcp_pair.first);
            EXPECT_EQ(echoed,
                      std::vector<std::uint8_t>(third_payload.begin(), third_payload.end()));

            tcp_pair.first.shutdown(asio::ip::tcp::socket::shutdown_send);
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });

    io_context.run();

    ASSERT_TRUE(relay_result.has_value());
    ASSERT_TRUE(*relay_result) << relay_result->error();
    EXPECT_EQ((*relay_result)->tls_to_udp_bytes, 7U);
    EXPECT_EQ((*relay_result)->udp_to_tls_bytes, 5U);
    EXPECT_EQ((*relay_result)->tls_to_udp_datagrams, 2U);
    EXPECT_EQ((*relay_result)->udp_to_tls_datagrams, 1U);
}

TEST(DatagramTest, PreservesDatagramBoundariesOverTcp) {
    asio::io_context io_context;
    auto tcp_pair = connected_pair(io_context);
    auto udp_pair = connected_udp_pair(io_context);
    std::optional<common::Result<DatagramRelayStats>> relay_result;

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            relay_result = co_await relay_tcp_and_udp(
                tcp_pair.second, udp_pair.second,
                {.inactivity_timeout = std::chrono::seconds{5}});
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });

    asio::co_spawn(
        io_context,
        [&]() -> asio::awaitable<void> {
            const std::vector<std::uint8_t> first(3U, 0x11U);
            const std::vector<std::uint8_t> second(11U, 0x22U);
            const auto first_record = frame_datagram(first);
            const auto second_record = frame_datagram(second);
            std::vector<std::uint8_t> combined;
            combined.reserve(first_record.size() + second_record.size());
            combined.insert(combined.end(), first_record.begin(), first_record.end());
            combined.insert(combined.end(), second_record.begin(), second_record.end());
            co_await asio::async_write(tcp_pair.first, asio::buffer(combined),
                                       asio::use_awaitable);

            std::array<std::uint8_t, kMaximumUdpPayloadSize> received{};
            const std::size_t first_len =
                co_await udp_pair.first.async_receive(asio::buffer(received), asio::use_awaitable);
            EXPECT_EQ(first_len, first.size());
            EXPECT_TRUE(std::equal(first.begin(), first.end(), received.begin()));
            const std::size_t second_len =
                co_await udp_pair.first.async_receive(asio::buffer(received), asio::use_awaitable);
            EXPECT_EQ(second_len, second.size());
            EXPECT_TRUE(std::equal(second.begin(), second.end(), received.begin()));

            tcp_pair.first.shutdown(asio::ip::tcp::socket::shutdown_send);
        },
        [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });

    io_context.run();

    ASSERT_TRUE(relay_result.has_value());
    ASSERT_TRUE(*relay_result) << relay_result->error();
    EXPECT_EQ((*relay_result)->tls_to_udp_datagrams, 2U);
    EXPECT_EQ((*relay_result)->tls_to_udp_bytes, 14U);
    EXPECT_EQ((*relay_result)->udp_to_tls_datagrams, 0U);
}

} // namespace
} // namespace minitun::protocol
