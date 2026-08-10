#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/frame.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::span<const std::uint8_t> as_bytes(const std::vector<std::uint8_t>& bytes) {
    return {bytes.data(), bytes.size()};
}

TEST(RemoteFrameTest, EncodesEveryHeaderFieldInNetworkByteOrder) {
    const Frame input{
        MessageType::register_tunnel,
        0U,
        0x0102030405060708ULL,
        {0xaaU, 0xbbU},
    };

    const auto encoded = encode_frame(input);

    ASSERT_TRUE(encoded) << encoded.error();
    ASSERT_EQ(encoded->size(), kFrameHeaderSize + 2U);
    EXPECT_EQ((*encoded)[0], 0x4dU);
    EXPECT_EQ((*encoded)[1], 0x54U);
    EXPECT_EQ((*encoded)[2], 0x55U);
    EXPECT_EQ((*encoded)[3], 0x4eU);
    EXPECT_EQ((*encoded)[4], 0x00U);
    EXPECT_EQ((*encoded)[5], 0x02U);
    EXPECT_EQ((*encoded)[6], 0x00U);
    EXPECT_EQ((*encoded)[7], 0x06U);
    EXPECT_EQ((*encoded)[8], 0x00U);
    EXPECT_EQ((*encoded)[9], 0x00U);
    EXPECT_EQ((*encoded)[10], 0x00U);
    EXPECT_EQ((*encoded)[11], 0x00U);
    EXPECT_EQ((*encoded)[12], 0x00U);
    EXPECT_EQ((*encoded)[13], 0x00U);
    EXPECT_EQ((*encoded)[14], 0x00U);
    EXPECT_EQ((*encoded)[15], 0x02U);
    for (std::size_t index = 0U; index < 8U; ++index) {
        EXPECT_EQ((*encoded)[16U + index], static_cast<std::uint8_t>(index + 1U));
    }
    EXPECT_EQ((*encoded)[24], 0xaaU);
    EXPECT_EQ((*encoded)[25], 0xbbU);
}

TEST(RemoteFrameTest, DecodesFragmentedAndCoalescedFramesIncludingEmptyPayload) {
    const Frame first{MessageType::hello, 0U, 1U, {}};
    const Frame second{MessageType::ping, 0U, 2U, {1U, 2U, 3U}};
    const Frame third{MessageType::goaway, 0U, 0U, {}};
    const auto first_encoded = encode_frame(first);
    const auto second_encoded = encode_frame(second);
    const auto third_encoded = encode_frame(third);
    ASSERT_TRUE(first_encoded);
    ASSERT_TRUE(second_encoded);
    ASSERT_TRUE(third_encoded);

    std::vector<std::uint8_t> stream;
    stream.insert(stream.end(), first_encoded->begin(), first_encoded->end());
    stream.insert(stream.end(), second_encoded->begin(), second_encoded->end());
    stream.insert(stream.end(), third_encoded->begin(), third_encoded->end());

    FrameDecoder decoder;
    std::vector<Frame> decoded;
    for (const std::uint8_t byte : stream) {
        const std::array fragment{byte};
        auto result = decoder.feed(fragment);
        ASSERT_TRUE(result) << result.error();
        decoded.insert(decoded.end(), result->begin(), result->end());
    }

    EXPECT_EQ(decoded, (std::vector<Frame>{first, second, third}));
    EXPECT_EQ(decoder.buffered_size(), 0U);
    EXPECT_TRUE(decoder.finish());
}

TEST(RemoteFrameTest, RejectsInvalidHeaderFieldsWithoutBufferingPayload) {
    const auto valid = encode_frame(Frame{MessageType::hello, 0U, 7U, {}});
    ASSERT_TRUE(valid);

    auto bad_magic = *valid;
    bad_magic[0] = 0U;
    FrameDecoder magic_decoder;
    auto magic_result = magic_decoder.feed(as_bytes(bad_magic));
    ASSERT_FALSE(magic_result);
    EXPECT_EQ(magic_result.error().code(), common::ErrorCode::protocol_error);

    auto bad_version = *valid;
    bad_version[5] = 1U;
    FrameDecoder version_decoder;
    auto version_result = version_decoder.feed(as_bytes(bad_version));
    ASSERT_FALSE(version_result);
    EXPECT_EQ(version_result.error().code(), common::ErrorCode::unsupported_version);

    auto bad_type = *valid;
    bad_type[6] = 0x7fU;
    bad_type[7] = 0xffU;
    FrameDecoder type_decoder;
    auto type_result = type_decoder.feed(as_bytes(bad_type));
    ASSERT_FALSE(type_result);
    EXPECT_EQ(type_result.error().code(), common::ErrorCode::protocol_error);

    auto bad_flags = *valid;
    bad_flags[11] = 1U;
    FrameDecoder flags_decoder;
    auto flags_result = flags_decoder.feed(as_bytes(bad_flags));
    ASSERT_FALSE(flags_result);
    EXPECT_EQ(flags_result.error().code(), common::ErrorCode::protocol_error);
}

TEST(RemoteFrameTest, RejectsMaliciousLengthsBeforeAllocatingTheirPayload) {
    auto header = encode_frame(Frame{MessageType::auth, 0U, 9U, {}});
    ASSERT_TRUE(header);
    (*header)[12] = 0x7fU;
    (*header)[13] = 0xffU;
    (*header)[14] = 0xffU;
    (*header)[15] = 0xffU;

    FrameDecoder decoder;
    const auto decoded = decoder.feed(as_bytes(*header));

    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code(), common::ErrorCode::frame_too_large);
    EXPECT_EQ(decoder.buffered_size(), 0U);
    EXPECT_FALSE(decoder.finish());
}

TEST(RemoteFrameTest, EnforcesConfiguredAndAbsoluteFrameLimits) {
    const Frame five_byte_payload{MessageType::ping, 0U, 1U, {1U, 2U, 3U, 4U, 5U}};
    const auto configured = encode_frame(five_byte_payload, kFrameHeaderSize + 4U);
    ASSERT_FALSE(configured);
    EXPECT_EQ(configured.error().code(), common::ErrorCode::frame_too_large);

    const Frame maximum{MessageType::ping, 0U, 1U, std::vector<std::uint8_t>(kMaxPayloadSize, 0U)};
    EXPECT_TRUE(encode_frame(maximum));

    Frame too_large = maximum;
    too_large.payload.push_back(0U);
    const auto absolute = encode_frame(too_large, kMaxFrameSize + 4096U);
    ASSERT_FALSE(absolute);
    EXPECT_EQ(absolute.error().code(), common::ErrorCode::frame_too_large);
}

TEST(RemoteFrameTest, RejectsTruncatedStreamsAndRequiresResetAfterFailure) {
    const auto encoded = encode_frame(Frame{MessageType::ping, 0U, 1U, {1U}});
    ASSERT_TRUE(encoded);

    FrameDecoder partial;
    ASSERT_TRUE(partial.feed({encoded->data(), encoded->size() - 1U}));
    const auto finished = partial.finish();
    ASSERT_FALSE(finished);
    EXPECT_EQ(finished.error().code(), common::ErrorCode::protocol_error);

    auto invalid = *encoded;
    invalid[0] = 0U;
    FrameDecoder failed;
    ASSERT_FALSE(failed.feed(as_bytes(invalid)));
    const auto still_failed = failed.feed(as_bytes(*encoded));
    ASSERT_FALSE(still_failed);
    EXPECT_EQ(still_failed.error().code(), common::ErrorCode::protocol_error);

    failed.reset();
    const auto recovered = failed.feed(as_bytes(*encoded));
    ASSERT_TRUE(recovered) << recovered.error();
    EXPECT_EQ(*recovered, (std::vector<Frame>{Frame{MessageType::ping, 0U, 1U, {1U}}}));
}

TEST(RemoteFrameTest, ClassifiesOnlyDefinedMessageTypes) {
    EXPECT_TRUE(is_control_message(MessageType::auth));
    EXPECT_FALSE(is_control_message(MessageType::worker_hello));
    EXPECT_TRUE(is_worker_message(MessageType::start_relay));
    EXPECT_FALSE(is_worker_message(MessageType::ping));
    EXPECT_EQ(message_type_from_wire(0xffffU), std::nullopt);
    EXPECT_EQ(to_string(MessageType::local_connect_ok), "LOCAL_CONNECT_OK");
}

TEST(RemoteFrameTest, RoundTripsAndNamesEveryDefinedMessageType) {
    constexpr std::array types{
        MessageType::hello,
        MessageType::hello_ack,
        MessageType::auth,
        MessageType::auth_ok,
        MessageType::auth_error,
        MessageType::register_tunnel,
        MessageType::register_tunnel_ok,
        MessageType::register_tunnel_error,
        MessageType::unregister_tunnel,
        MessageType::unregister_tunnel_ok,
        MessageType::request_workers,
        MessageType::ping,
        MessageType::pong,
        MessageType::goaway,
        MessageType::error,
        MessageType::worker_hello,
        MessageType::worker_accepted,
        MessageType::start_relay,
        MessageType::local_connect_ok,
        MessageType::local_connect_error,
    };
    constexpr std::array<std::string_view, types.size()> names{
        "HELLO",
        "HELLO_ACK",
        "AUTH",
        "AUTH_OK",
        "AUTH_ERROR",
        "REGISTER_TUNNEL",
        "REGISTER_TUNNEL_OK",
        "REGISTER_TUNNEL_ERROR",
        "UNREGISTER_TUNNEL",
        "UNREGISTER_TUNNEL_OK",
        "REQUEST_WORKERS",
        "PING",
        "PONG",
        "GOAWAY",
        "ERROR",
        "WORKER_HELLO",
        "WORKER_ACCEPTED",
        "START_RELAY",
        "LOCAL_CONNECT_OK",
        "LOCAL_CONNECT_ERROR",
    };
    for (std::size_t index = 0U; index < types.size(); ++index) {
        EXPECT_EQ(to_string(types[index]), names[index]);
        EXPECT_EQ(message_type_from_wire(static_cast<std::uint16_t>(types[index])), types[index]);
    }
    EXPECT_EQ(to_string(static_cast<MessageType>(0U)), "UNKNOWN");
    EXPECT_FALSE(message_type_from_wire(0U));
    EXPECT_FALSE(is_control_message(static_cast<MessageType>(0U)));
    EXPECT_FALSE(is_control_message(static_cast<MessageType>(0xffffU)));
    EXPECT_FALSE(is_worker_message(static_cast<MessageType>(0U)));
    EXPECT_FALSE(is_worker_message(static_cast<MessageType>(0xffffU)));
}

TEST(RemoteFrameTest, RejectsInvalidEncodeTypeAndRetainsSpecificDecoderFailures) {
    Frame invalid_type;
    invalid_type.type = static_cast<MessageType>(0U);
    const auto encoded = encode_frame(invalid_type);
    ASSERT_FALSE(encoded);
    EXPECT_EQ(encoded.error().code(), common::ErrorCode::protocol_error);

    const auto valid = encode_frame(Frame{MessageType::hello, 0U, 0U, {}});
    ASSERT_TRUE(valid) << valid.error();
    auto bad_version = *valid;
    bad_version[5] = 1U;
    FrameDecoder unsupported;
    ASSERT_FALSE(unsupported.feed(bad_version));
    const auto still_unsupported = unsupported.feed(*valid);
    ASSERT_FALSE(still_unsupported);
    EXPECT_EQ(still_unsupported.error().code(), common::ErrorCode::unsupported_version);

    auto large = *valid;
    large[12] = 1U;
    FrameDecoder bounded{kFrameHeaderSize};
    ASSERT_FALSE(bounded.feed(large));
    const auto still_large = bounded.feed(*valid);
    ASSERT_FALSE(still_large);
    EXPECT_EQ(still_large.error().code(), common::ErrorCode::frame_too_large);
}

TEST(RemoteFrameTest, ClampsDecoderLimitsAndDistinguishesPartialHeaderFromPayload) {
    FrameDecoder minimum{0U};
    EXPECT_EQ(minimum.max_frame_size(), kFrameHeaderSize);
    FrameDecoder maximum{kMaxFrameSize + 1U};
    EXPECT_EQ(maximum.max_frame_size(), kMaxFrameSize);

    const auto encoded = encode_frame(Frame{MessageType::ping, 0U, 1U, {1U}});
    ASSERT_TRUE(encoded) << encoded.error();
    FrameDecoder partial_header;
    ASSERT_TRUE(partial_header.feed({encoded->data(), 1U}));
    EXPECT_EQ(partial_header.buffered_size(), 1U);
    EXPECT_FALSE(partial_header.finish());
}

} // namespace
} // namespace minitun::protocol
