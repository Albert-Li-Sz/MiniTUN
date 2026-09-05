#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include <minitun/server/tls_admission.hpp>

namespace minitun::server {
namespace {

using namespace std::chrono_literals;

TEST(TlsAdmissionTest, BoundsPendingConnectionsPerSourceAndGloballyUntilReleased) {
    TlsAdmission admission{3U, 2U, 10U};
    const auto now = TlsAdmission::Clock::time_point{};
    EXPECT_FALSE(admission.try_acquire("", now));
    EXPECT_TRUE(admission.try_acquire("192.0.2.1", now));
    EXPECT_TRUE(admission.try_acquire("192.0.2.1", now));
    EXPECT_FALSE(admission.try_acquire("192.0.2.1", now));
    EXPECT_TRUE(admission.try_acquire("2001:db8::1", now));
    EXPECT_FALSE(admission.try_acquire("192.0.2.2", now));
    EXPECT_EQ(admission.pending(), 3U);
    EXPECT_GT(admission.accept_delay(now), 0ns);

    admission.release("192.0.2.1");
    EXPECT_EQ(admission.accept_delay(now), 0ns);
    EXPECT_TRUE(admission.try_acquire("192.0.2.2", now));
    admission.release("192.0.2.1");
    admission.release("192.0.2.1");
    admission.release("2001:db8::1");
    admission.release("192.0.2.2");
    EXPECT_EQ(admission.pending(), 0U);
    EXPECT_TRUE(admission.try_acquire("192.0.2.1", now));
}

TEST(TlsAdmissionTest, PacesAcceptedSocketsEvenWhenAdmissionRejectsThem) {
    TlsAdmission admission{2U, 1U, 4U};
    const auto now = TlsAdmission::Clock::time_point{};
    ASSERT_TRUE(admission.try_acquire("192.0.2.1", now));
    for (int index = 0; index < 4; ++index) {
        EXPECT_EQ(admission.accept_delay(now), 0ns);
        admission.record_accept(now);
        EXPECT_FALSE(admission.try_acquire("192.0.2.1", now));
    }
    EXPECT_EQ(admission.accept_delay(now), 250ms);
    EXPECT_EQ(admission.accept_delay(now + 125ms), 125ms);
    EXPECT_EQ(admission.accept_delay(now + 250ms), 0ns);
    admission.record_accept(now + 250ms);
    EXPECT_EQ(admission.accept_delay(now + 250ms), 250ms);
    // An idle accept replenishes only the bounded burst, not unlimited credit.
    for (int index = 0; index < 4; ++index) {
        admission.record_accept(now + 1h);
    }
    EXPECT_EQ(admission.accept_delay(now + 1h), 250ms);
}

TEST(TlsAdmissionTest, RepeatedTlsFailuresBlockOnlyTheirSourceAndExpire) {
    TlsAdmission admission{8U, 4U, 100U};
    const auto now = TlsAdmission::Clock::time_point{};
    for (int index = 0; index < 5; ++index) {
        EXPECT_TRUE(admission.try_acquire("192.0.2.1", now));
        static_cast<void>(admission.record_failure("192.0.2.1", now));
        admission.release("192.0.2.1");
    }
    EXPECT_FALSE(admission.try_acquire("192.0.2.1", now + 59s));
    EXPECT_TRUE(admission.try_acquire("192.0.2.2", now));
    EXPECT_TRUE(admission.try_acquire("192.0.2.1", now + 60s));
}

TEST(TlsAdmissionTest, LimitsDiagnosticsAcrossSourcesWithoutResettingOnSuccessfulAccepts) {
    TlsAdmission admission{128U, 32U, 100U};
    const auto now = TlsAdmission::Clock::time_point{};
    EXPECT_EQ(admission.record_failure("192.0.2.1", now), 0U);
    for (int index = 0; index < 100; ++index) {
        admission.record_accept(now + 1s);
        EXPECT_FALSE(admission.record_failure("2001:db8::" + std::to_string(index), now + 1s));
    }
    EXPECT_FALSE(admission.record_failure("192.0.2.2", now + 4999ms));
    EXPECT_EQ(admission.record_failure("192.0.2.3", now + 5s), 101U);
    EXPECT_EQ(admission.record_failure("192.0.2.4", now + 10s), 0U);
}

TEST(TlsAdmissionTest, BoundsCreditAtHighRatesAndIgnoresClockRegression) {
    TlsAdmission admission{128U, 32U, 100'000U};
    const auto now = TlsAdmission::Clock::time_point{10s};
    for (int index = 0; index < 100'000; ++index) {
        admission.record_accept(now);
    }
    EXPECT_EQ(admission.accept_delay(now), 10us);
    EXPECT_EQ(admission.accept_delay(now - 1s), 10us);
    EXPECT_EQ(admission.accept_delay(now + 10us), 0ns);
}

} // namespace
} // namespace minitun::server
