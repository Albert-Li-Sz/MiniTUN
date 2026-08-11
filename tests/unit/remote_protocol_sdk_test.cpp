#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/remote_protocol.hpp>

namespace minitun::remote {
namespace {

TEST(RemoteProtocolSdkTest, RoundTripsTypedFramesIncrementally) {
    const protocol::RegisterTunnelMessage registration{
        "tun_00000000000000000000000000000001", "127.0.0.1", 6500U, 9U, protocol::TunnelMode::udp};
    const Message message{registration};
    auto frame = Codec::make_frame(protocol::MessageType::register_tunnel, 42U, message);
    ASSERT_TRUE(frame) << frame.error();
    auto bytes = Codec::encode_frame(*frame);
    ASSERT_TRUE(bytes) << bytes.error();

    Decoder decoder;
    std::vector<protocol::Frame> decoded;
    for (const std::uint8_t byte : *bytes) {
        const std::array one{byte};
        auto fragment = decoder.feed(one);
        ASSERT_TRUE(fragment) << fragment.error();
        decoded.insert(decoded.end(), fragment->begin(), fragment->end());
    }
    ASSERT_TRUE(decoder.finish());
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded[0], *frame);
    auto typed = Codec::decode_message(decoded[0]);
    ASSERT_TRUE(typed) << typed.error();
    ASSERT_TRUE(std::holds_alternative<protocol::RegisterTunnelMessage>(*typed));
    EXPECT_EQ(std::get<protocol::RegisterTunnelMessage>(*typed), registration);
}

TEST(RemoteProtocolSdkTest, RejectsMismatchedTypedMessagesAndPartialFrames) {
    const Message heartbeat{protocol::HeartbeatMessage{7U}};
    const auto mismatch = Codec::make_frame(protocol::MessageType::hello, 1U, heartbeat);
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code(), common::ErrorCode::invalid_argument);

    Decoder decoder;
    const std::array<std::uint8_t, 3U> partial{0x4dU, 0x54U, 0x55U};
    ASSERT_TRUE(decoder.feed(partial));
    const auto finished = decoder.finish();
    ASSERT_FALSE(finished);
    EXPECT_EQ(finished.error().code(), common::ErrorCode::protocol_error);
    decoder.reset();
    EXPECT_TRUE(decoder.finish());
}

TEST(RemoteProtocolSdkTest, MovedFromDecoderRemainsSafelyDestructible) {
    Decoder source;
    Decoder destination{std::move(source)};
    EXPECT_TRUE(destination.finish());
    const auto feed = source.feed({});
    ASSERT_FALSE(feed);
    EXPECT_EQ(feed.error().code(), common::ErrorCode::invalid_argument);
    const auto finish = source.finish();
    ASSERT_FALSE(finish);
    EXPECT_EQ(finish.error().code(), common::ErrorCode::invalid_argument);
    source.reset();
}

} // namespace
} // namespace minitun::remote
