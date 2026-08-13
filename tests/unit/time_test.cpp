#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/time.hpp>

namespace minitun::common {
namespace {

using namespace std::chrono_literals;

TEST(TimeTest, ConvertsSystemTimeToUnixUnits) {
    EXPECT_EQ(to_unix_seconds(SystemClock::time_point{}), 0);
    EXPECT_EQ(to_unix_milliseconds(SystemClock::time_point{}), 0);

    const auto positive = SystemClock::time_point{} + 1'234ms;
    EXPECT_EQ(to_unix_seconds(positive), 1);
    EXPECT_EQ(to_unix_milliseconds(positive), 1'234);
}

TEST(TimeTest, FloorsNegativeFractionalUnixTimes) {
    const auto one_millisecond_before_epoch = SystemClock::time_point{} - 1ms;
    EXPECT_EQ(to_unix_seconds(one_millisecond_before_epoch), -1);
    EXPECT_EQ(to_unix_milliseconds(one_millisecond_before_epoch), -1);

    const auto mixed_negative = SystemClock::time_point{} - 1'234ms;
    EXPECT_EQ(to_unix_seconds(mixed_negative), -2);
    EXPECT_EQ(to_unix_milliseconds(mixed_negative), -1'234);
}

TEST(TimeTest, NowFunctionsReturnValuesInsideObservedBounds) {
    const auto system_before = SystemClock::now();
    const std::int64_t seconds = unix_seconds_now();
    const std::int64_t milliseconds = unix_milliseconds_now();
    const auto system_after = SystemClock::now();

    EXPECT_GE(seconds, to_unix_seconds(system_before));
    EXPECT_LE(seconds, to_unix_seconds(system_after));
    EXPECT_GE(milliseconds, to_unix_milliseconds(system_before));
    EXPECT_LE(milliseconds, to_unix_milliseconds(system_after));

    const auto steady_before = SteadyClock::now();
    const auto observed_steady = steady_now();
    const auto steady_after = SteadyClock::now();
    EXPECT_GE(observed_steady, steady_before);
    EXPECT_LE(observed_steady, steady_after);
}

TEST(TimeTest, ComputesClockSkewWithoutOverflow) {
    EXPECT_EQ(absolute_clock_skew_seconds(100, 90), 10U);
    EXPECT_EQ(absolute_clock_skew_seconds(90, 100), 10U);
    EXPECT_EQ(absolute_clock_skew_seconds(-10, -20), 10U);
    EXPECT_EQ(absolute_clock_skew_seconds(-10, 10), 20U);
    EXPECT_EQ(absolute_clock_skew_seconds(std::numeric_limits<std::int64_t>::min(),
                                          std::numeric_limits<std::int64_t>::max()),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(TimeTest, ClockSkewCheckIsInclusiveAndDeterministic) {
    const auto inside = is_clock_skew_within(1'000, 900, 100s);
    ASSERT_TRUE(inside);
    EXPECT_TRUE(*inside);

    const auto outside = is_clock_skew_within(1'001, 900, 100s);
    ASSERT_TRUE(outside);
    EXPECT_FALSE(*outside);

    const auto negative_times = is_clock_skew_within(-101, -1, 100s);
    ASSERT_TRUE(negative_times);
    EXPECT_TRUE(*negative_times);

    const auto extreme =
        is_clock_skew_within(std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max(), std::chrono::seconds::max());
    ASSERT_TRUE(extreme);
    EXPECT_FALSE(*extreme);
}

TEST(TimeTest, ClockSkewCheckRejectsNegativeTolerance) {
    const auto result = is_clock_skew_within(10, 10, -1s);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::invalid_argument);
}

TEST(TimeTest, FormatsUtcTimestampsIncludingBeforeTheEpoch) {
    const auto epoch = format_unix_time_utc(0);
    ASSERT_TRUE(epoch) << epoch.error();
    EXPECT_EQ(*epoch, "1970-01-01T00:00:00Z");

    const auto before_epoch = format_unix_time_utc(-1);
    ASSERT_TRUE(before_epoch) << before_epoch.error();
    EXPECT_EQ(*before_epoch, "1969-12-31T23:59:59Z");

    const auto leap_day = format_unix_time_utc(951'827'696);
    ASSERT_TRUE(leap_day) << leap_day.error();
    EXPECT_EQ(*leap_day, "2000-02-29T12:34:56Z");

    const auto extreme = format_unix_time_utc(std::numeric_limits<std::int64_t>::max());
    EXPECT_FALSE(extreme);
    EXPECT_EQ(extreme.error().code(), ErrorCode::invalid_argument);
}

TEST(TimeTest, UtcFormattingIsSafeForConcurrentCallers) {
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t iterations = 250;
    std::atomic<bool> all_correct{true};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&all_correct] {
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                const auto formatted = format_unix_time_utc(951'827'696);
                if (!formatted || *formatted != "2000-02-29T12:34:56Z") {
                    all_correct.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_TRUE(all_correct.load(std::memory_order_relaxed));
}

} // namespace
} // namespace minitun::common
