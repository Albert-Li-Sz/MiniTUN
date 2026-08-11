#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ssl/context.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/datagram.hpp>

namespace minitun::protocol {
namespace {

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

} // namespace
} // namespace minitun::protocol
