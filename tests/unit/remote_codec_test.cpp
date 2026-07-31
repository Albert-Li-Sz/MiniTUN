#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/codec.hpp>

namespace minitun::protocol {
namespace {

TEST(RemoteCodecTest, RoundTripsBoundedFieldsInNetworkByteOrder) {
    PayloadWriter writer;
    ASSERT_TRUE(writer.write_u8(0x7fU));
    ASSERT_TRUE(writer.write_u16(0x0102U));
    ASSERT_TRUE(writer.write_u32(0x03040506U));
    ASSERT_TRUE(writer.write_u64(0x0708090a0b0c0d0eULL));
    constexpr std::array<std::uint8_t, 2U> binary{0U, 0xffU};
    ASSERT_TRUE(writer.write_bytes(binary));
    ASSERT_TRUE(writer.write_string("MiniTun \xc3\xa9"));
    auto payload = std::move(writer).finish();
    ASSERT_TRUE(payload) << payload.error();

    EXPECT_EQ((*payload)[1], 0x01U);
    EXPECT_EQ((*payload)[2], 0x02U);
    EXPECT_EQ((*payload)[3], 0x03U);
    EXPECT_EQ((*payload)[4], 0x04U);
    EXPECT_EQ((*payload)[5], 0x05U);
    EXPECT_EQ((*payload)[6], 0x06U);

    PayloadReader reader{*payload};
    EXPECT_EQ(*reader.read_u8(), 0x7fU);
    EXPECT_EQ(*reader.read_u16(), 0x0102U);
    EXPECT_EQ(*reader.read_u32(), 0x03040506U);
    EXPECT_EQ(*reader.read_u64(), 0x0708090a0b0c0d0eULL);
    EXPECT_EQ(*reader.read_bytes(2U), (std::vector<std::uint8_t>{0U, 0xffU}));
    EXPECT_EQ(*reader.read_string(32U), "MiniTun \xc3\xa9");
    EXPECT_TRUE(reader.require_end());
}

TEST(RemoteCodecTest, WriterFailureIsStickyAndProducesNoPartialPayload) {
    PayloadWriter writer{4U};
    EXPECT_TRUE(writer.write_u32(1U));
    const auto overflow = writer.write_u8(2U);
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code(), common::ErrorCode::frame_too_large);
    EXPECT_FALSE(writer.write_u8(3U));

    const auto payload = std::move(writer).finish();
    ASSERT_FALSE(payload);
    EXPECT_EQ(payload.error().code(), common::ErrorCode::frame_too_large);
}

TEST(RemoteCodecTest, RejectsTruncatedOversizedAndTrailingFields) {
    constexpr std::array<std::uint8_t, 1U> one_byte{0x01U};
    PayloadReader truncated{one_byte};
    const auto integer = truncated.read_u16();
    ASSERT_FALSE(integer);
    EXPECT_EQ(integer.error().code(), common::ErrorCode::protocol_error);

    constexpr std::array<std::uint8_t, 4U> oversized{0U, 2U, 'a', 'b'};
    PayloadReader bounded{oversized};
    const auto bytes = bounded.read_bytes(1U);
    ASSERT_FALSE(bytes);
    EXPECT_EQ(bytes.error().code(), common::ErrorCode::protocol_error);

    PayloadReader trailing{one_byte};
    const auto end = trailing.require_end();
    ASSERT_FALSE(end);
    EXPECT_EQ(end.error().code(), common::ErrorCode::protocol_error);
}

TEST(RemoteCodecTest, StrictlyValidatesUtf8AndEmbeddedNul) {
    EXPECT_TRUE(is_valid_utf8("ASCII"));
    EXPECT_TRUE(is_valid_utf8("\xf0\x9f\x9a\x87"));
    EXPECT_FALSE(is_valid_utf8("\xc0\x80"));
    EXPECT_FALSE(is_valid_utf8("\xed\xa0\x80"));
    EXPECT_FALSE(is_valid_utf8("\xf4\x90\x80\x80"));
    EXPECT_FALSE(is_valid_utf8("\xe2\x82"));

    PayloadWriter invalid_utf8;
    const auto invalid_write = invalid_utf8.write_string("\xc0\x80");
    ASSERT_FALSE(invalid_write);
    EXPECT_EQ(invalid_write.error().code(), common::ErrorCode::invalid_argument);

    const std::string with_nul{"a\0b", 3U};
    PayloadWriter embedded_nul;
    EXPECT_FALSE(embedded_nul.write_string(with_nul));

    constexpr std::array<std::uint8_t, 4U> invalid_wire{0U, 2U, 0xc0U, 0x80U};
    PayloadReader reader{invalid_wire};
    const auto invalid_read = reader.read_string(2U);
    ASSERT_FALSE(invalid_read);
    EXPECT_EQ(invalid_read.error().code(), common::ErrorCode::protocol_error);
}

} // namespace
} // namespace minitun::protocol
