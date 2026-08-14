#include <chrono>

#include <gtest/gtest.h>

#include <minitun/daemon/reconnect_backoff.hpp>

namespace minitun::daemon {
namespace {

TEST(ReconnectBackoffTest, UsesSpecifiedExponentialSequenceAndCap) {
    using namespace std::chrono_literals;
    EXPECT_EQ(ReconnectBackoff::base_delay(0U), 1s);
    EXPECT_EQ(ReconnectBackoff::base_delay(1U), 2s);
    EXPECT_EQ(ReconnectBackoff::base_delay(2U), 4s);
    EXPECT_EQ(ReconnectBackoff::base_delay(3U), 8s);
    EXPECT_EQ(ReconnectBackoff::base_delay(4U), 16s);
    EXPECT_EQ(ReconnectBackoff::base_delay(5U), 30s);
    EXPECT_EQ(ReconnectBackoff::base_delay(100U), 30s);
}

TEST(ReconnectBackoffTest, AppliesInclusiveTwentyPercentJitter) {
    using namespace std::chrono_literals;
    EXPECT_EQ(ReconnectBackoff::apply_jitter(10s, 0.0), 8s);
    EXPECT_EQ(ReconnectBackoff::apply_jitter(10s, 0.5), 10s);
    EXPECT_EQ(ReconnectBackoff::apply_jitter(10s, 1.0), 12s);
    EXPECT_EQ(ReconnectBackoff::apply_jitter(10s, -1.0), 8s);
    EXPECT_EQ(ReconnectBackoff::apply_jitter(10s, 2.0), 12s);
}

TEST(ReconnectBackoffTest, AdvancesAttemptsAndResetsAfterSuccess) {
    ReconnectBackoff backoff{123U};
    for (std::uint32_t expected = 1U; expected <= 8U; ++expected) {
        const auto delay = backoff.next_delay();
        EXPECT_GE(delay, ReconnectBackoff::base_delay(expected - 1U) * 8 / 10);
        EXPECT_LE(delay, ReconnectBackoff::base_delay(expected - 1U) * 12 / 10);
        EXPECT_EQ(backoff.attempt(), expected);
    }
    backoff.reset();
    EXPECT_EQ(backoff.attempt(), 0U);
}

} // namespace
} // namespace minitun::daemon
