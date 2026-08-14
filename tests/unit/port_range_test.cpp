#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/port_range.hpp>

namespace minitun::common {
namespace {

TEST(PortRangeTest, ParsesSinglePortAsOneElementRange) {
    const auto range = PortRange::parse("443");

    ASSERT_TRUE(range);
    EXPECT_EQ(range->start(), 443);
    EXPECT_EQ(range->end(), 443);
    EXPECT_EQ(range->size(), std::uint32_t{1});
    EXPECT_TRUE(range->contains(443));
    EXPECT_FALSE(range->contains(442));
    EXPECT_EQ(range->to_string(), "443");
}

TEST(PortRangeTest, ParsesInclusiveRangeAndChecksBoundaries) {
    const auto range = PortRange::parse("6000-6999");

    ASSERT_TRUE(range);
    EXPECT_EQ(range->start(), 6000);
    EXPECT_EQ(range->end(), 6999);
    EXPECT_EQ(range->size(), std::uint32_t{1000});
    EXPECT_TRUE(range->contains(6000));
    EXPECT_TRUE(range->contains(6500));
    EXPECT_TRUE(range->contains(6999));
    EXPECT_FALSE(range->contains(5999));
    EXPECT_FALSE(range->contains(7000));
}

TEST(PortRangeTest, SupportsEntireValidPortDomainWithoutOverflow) {
    const auto range = PortRange::parse("1-65535");

    ASSERT_TRUE(range);
    EXPECT_EQ(range->size(), std::uint32_t{65535});
    EXPECT_TRUE(range->contains(1));
    EXPECT_TRUE(range->contains(65535));
    EXPECT_FALSE(range->contains(0));
}

TEST(PortRangeTest, CanonicalOutputRoundTrips) {
    constexpr std::array<std::string_view, 4> inputs{
        "1",
        "65535",
        "1-2",
        "6000-6999",
    };

    for (const std::string_view input : inputs) {
        const auto parsed = PortRange::parse(input);
        ASSERT_TRUE(parsed) << input;

        const auto reparsed = PortRange::parse(parsed->to_string());
        ASSERT_TRUE(reparsed) << input;
        EXPECT_EQ(*reparsed, *parsed) << input;
    }
}

TEST(PortRangeTest, RejectsZeroOverflowAndReverseRanges) {
    constexpr std::array<std::string_view, 10> invalid{
        "0",
        "65536",
        "999999999999999999999999",
        "0-1",
        "1-65536",
        "65536-65536",
        "65535-1",
        "100-99",
        "999999999999999999999999-65535",
        "1-999999999999999999999999",
    };

    for (const std::string_view input : invalid) {
        const auto parsed = PortRange::parse(input);
        ASSERT_FALSE(parsed) << input;
        EXPECT_EQ(parsed.error().code(), ErrorCode::invalid_argument) << input;
    }
}

TEST(PortRangeTest, RejectsWhitespaceAndMalformedSyntax) {
    constexpr std::array<std::string_view, 18> invalid{
        "",   " 80",   "80 ", "6 000", "1 -2",  "1- 2", "\t1-2", "1-2\n",  "-1",
        "1-", "1-2-3", "+80", "0x50",  "eight", "1--2", "--",    "000080", "00001-000002",
    };

    for (const std::string_view input : invalid) {
        EXPECT_FALSE(PortRange::parse(input)) << input;
    }
}

TEST(PortRangeTest, CanonicalizesDecimalLeadingZeroes) {
    const auto single = PortRange::parse("00080");
    const auto range = PortRange::parse("00001-00002");

    ASSERT_TRUE(single);
    EXPECT_EQ(single->to_string(), "80");
    ASSERT_TRUE(range);
    EXPECT_EQ(range->to_string(), "1-2");
}

} // namespace
} // namespace minitun::common
