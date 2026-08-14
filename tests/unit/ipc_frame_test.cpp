#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/ipc/frame.hpp>

namespace minitun::ipc {
namespace {

[[nodiscard]] std::span<const std::uint8_t> as_bytes(const std::vector<std::uint8_t>& bytes) {
    return {bytes.data(), bytes.size()};
}

TEST(IpcFrameTest, EncodesPayloadLengthInNetworkByteOrder) {
    const std::string payload(258U, 'x');

    const auto frame = encode_frame(payload);

    ASSERT_TRUE(frame) << frame.error();
    ASSERT_EQ(frame->size(), kFrameHeaderSize + payload.size());
    EXPECT_EQ((*frame)[0], std::uint8_t{0x00});
    EXPECT_EQ((*frame)[1], std::uint8_t{0x00});
    EXPECT_EQ((*frame)[2], std::uint8_t{0x01});
    EXPECT_EQ((*frame)[3], std::uint8_t{0x02});
    EXPECT_EQ(
        std::string(frame->begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize), frame->end()),
        payload);
}

TEST(IpcFrameTest, RejectsAZeroLengthFrame) {
    const auto encoded = encode_frame("");
    ASSERT_FALSE(encoded);
    EXPECT_EQ(encoded.error().code(), common::ErrorCode::protocol_error);

    constexpr std::array<std::uint8_t, kFrameHeaderSize> empty{0U, 0U, 0U, 0U};
    FrameDecoder decoder;
    const auto decoded = decoder.feed(empty);

    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code(), common::ErrorCode::protocol_error);
    EXPECT_FALSE(decoder.finish());
}

TEST(IpcFrameTest, DecodesHeaderAndPayloadDeliveredOneByteAtATime) {
    constexpr std::string_view payload{"fragmented"};
    const auto frame = encode_frame(payload);
    ASSERT_TRUE(frame) << frame.error();

    FrameDecoder decoder;
    std::vector<std::string> completed;
    for (const std::uint8_t byte : *frame) {
        const std::array fragment{byte};
        auto decoded = decoder.feed(fragment);
        ASSERT_TRUE(decoded) << decoded.error();
        completed.insert(completed.end(), decoded->begin(), decoded->end());
    }

    ASSERT_EQ(completed.size(), 1U);
    EXPECT_EQ(completed.front(), payload);
    EXPECT_EQ(decoder.buffered_size(), 0U);
    EXPECT_TRUE(decoder.finish());
}

TEST(IpcFrameTest, DecodesMultipleCoalescedFrames) {
    const auto first = encode_frame("one");
    const auto second = encode_frame("second");
    const auto third = encode_frame("three");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);

    std::vector<std::uint8_t> stream;
    stream.insert(stream.end(), first->begin(), first->end());
    stream.insert(stream.end(), second->begin(), second->end());
    stream.insert(stream.end(), third->begin(), third->end());

    FrameDecoder decoder;
    const auto decoded = decoder.feed(as_bytes(stream));

    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_EQ(*decoded, (std::vector<std::string>{"one", "second", "three"}));
    EXPECT_TRUE(decoder.finish());
}

TEST(IpcFrameTest, PreservesBinaryPayloadBytes) {
    const std::string payload{"a\0\xffz", 4U};
    const auto frame = encode_frame(payload);
    ASSERT_TRUE(frame) << frame.error();

    FrameDecoder decoder;
    const auto decoded = decoder.feed(as_bytes(*frame));

    ASSERT_TRUE(decoded) << decoded.error();
    ASSERT_EQ(decoded->size(), 1U);
    EXPECT_EQ(decoded->front(), payload);
}

TEST(IpcFrameTest, RejectsPayloadBeyondTheConfiguredOrAbsoluteLimit) {
    const std::string five_bytes(5U, 'x');
    const auto configured_limit = encode_frame(five_bytes, 4U);
    ASSERT_FALSE(configured_limit);
    EXPECT_EQ(configured_limit.error().code(), common::ErrorCode::frame_too_large);

    const std::string absolute_limit_plus_one(kDefaultMaxFrameSize + 1U, 'x');
    const auto absolute_limit = encode_frame(absolute_limit_plus_one, kDefaultMaxFrameSize + 4096U);
    ASSERT_FALSE(absolute_limit);
    EXPECT_EQ(absolute_limit.error().code(), common::ErrorCode::frame_too_large);
}

TEST(IpcFrameTest, RejectsAnOversizedLengthBeforeBufferingItsPayload) {
    constexpr std::array<std::uint8_t, kFrameHeaderSize> length_five{0U, 0U, 0U, 5U};
    FrameDecoder decoder{4U};

    const auto decoded = decoder.feed(length_five);

    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code(), common::ErrorCode::frame_too_large);
    EXPECT_EQ(decoder.buffered_size(), 0U);
    EXPECT_EQ(decoder.max_frame_size(), 4U);
}

TEST(IpcFrameTest, RemainsFailedUntilExplicitlyReset) {
    constexpr std::array<std::uint8_t, kFrameHeaderSize> oversized{0U, 0U, 0U, 2U};
    FrameDecoder decoder{1U};
    ASSERT_FALSE(decoder.feed(oversized));

    constexpr std::array<std::uint8_t, 5> valid{0U, 0U, 0U, 1U, static_cast<std::uint8_t>('x')};
    const auto still_failed = decoder.feed(valid);
    ASSERT_FALSE(still_failed);
    EXPECT_EQ(still_failed.error().code(), common::ErrorCode::frame_too_large);
    EXPECT_FALSE(decoder.finish());

    decoder.reset();
    const auto recovered = decoder.feed(valid);
    ASSERT_TRUE(recovered) << recovered.error();
    ASSERT_EQ(recovered->size(), 1U);
    EXPECT_EQ(recovered->front(), "x");
    EXPECT_TRUE(decoder.finish());
}

TEST(IpcFrameTest, FinishRejectsPartialHeaderAndPartialPayload) {
    FrameDecoder partial_header;
    constexpr std::array<std::uint8_t, 2> header_fragment{0U, 0U};
    ASSERT_TRUE(partial_header.feed(header_fragment));
    const auto header_finished = partial_header.finish();
    ASSERT_FALSE(header_finished);
    EXPECT_EQ(header_finished.error().code(), common::ErrorCode::protocol_error);

    FrameDecoder partial_payload;
    constexpr std::array<std::uint8_t, 5> payload_fragment{0U, 0U, 0U, 2U,
                                                           static_cast<std::uint8_t>('x')};
    ASSERT_TRUE(partial_payload.feed(payload_fragment));
    const auto payload_finished = partial_payload.finish();
    ASSERT_FALSE(payload_finished);
    EXPECT_EQ(payload_finished.error().code(), common::ErrorCode::protocol_error);
}

} // namespace
} // namespace minitun::ipc
