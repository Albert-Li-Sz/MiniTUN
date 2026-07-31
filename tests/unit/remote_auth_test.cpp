#include <chrono>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/protocol/auth.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::string client_id() {
    auto id = common::Id::generate(common::IdKind::client);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

TEST(RemoteAuthTest, ComputesDeterministicHmacAndDetectsEveryChangedInput) {
    AuthenticationNonce nonce{};
    nonce.fill(0x5aU);
    const std::string id = client_id();
    const auto first = compute_authentication_data("correct horse", id, 123456, nonce);
    const auto second = compute_authentication_data("correct horse", id, 123456, nonce);
    ASSERT_TRUE(first) << first.error();
    ASSERT_TRUE(second) << second.error();
    EXPECT_EQ(*first, *second);

    EXPECT_EQ(*verify_authentication_data("correct horse", id, 123456, nonce, *first), true);
    EXPECT_EQ(*verify_authentication_data("wrong horse", id, 123456, nonce, *first), false);
    EXPECT_EQ(*verify_authentication_data("correct horse", id, 123457, nonce, *first), false);
    ++nonce[0];
    EXPECT_EQ(*verify_authentication_data("correct horse", id, 123456, nonce, *first), false);
}

TEST(RemoteAuthTest, RejectsEmptyTokenAndInvalidClientIdentifier) {
    AuthenticationNonce nonce{};
    const auto empty = compute_authentication_data("", client_id(), 0, nonce);
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), common::ErrorCode::invalid_argument);

    const auto invalid = compute_authentication_data("token", "not-a-client", 0, nonce);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), common::ErrorCode::invalid_argument);
}

TEST(RemoteAuthTest, GeneratesNonZeroIndependentCryptographicValues) {
    const auto first_nonce = generate_authentication_nonce();
    const auto second_nonce = generate_authentication_nonce();
    const auto first_generation = generate_session_generation();
    const auto second_generation = generate_session_generation();
    ASSERT_TRUE(first_nonce);
    ASSERT_TRUE(second_nonce);
    ASSERT_TRUE(first_generation);
    ASSERT_TRUE(second_generation);
    EXPECT_NE(*first_nonce, *second_nonce);
    EXPECT_NE(*first_generation, 0U);
    EXPECT_NE(*second_generation, 0U);
    EXPECT_NE(*first_generation, *second_generation);
}

TEST(RemoteAuthTest, ReplayCacheRejectsDuplicatesExpiresAndStaysBounded) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{std::chrono::seconds{100}};
    NonceReplayCache cache{{.max_entries = 1U, .retention = std::chrono::seconds{10}}};
    AuthenticationNonce first{};
    AuthenticationNonce second{};
    second[0] = 1U;

    EXPECT_EQ(*cache.consume(first, now), true);
    EXPECT_EQ(*cache.consume(first, now + std::chrono::seconds{1}), false);
    const auto full = cache.consume(second, now + std::chrono::seconds{1});
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code(), common::ErrorCode::resource_exhausted);
    EXPECT_EQ(cache.size(), 1U);
    EXPECT_EQ(*cache.consume(second, now + std::chrono::seconds{11}), true);
}

TEST(RemoteAuthTest, RateLimiterBlocksAtThresholdAndRecovers) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{std::chrono::seconds{100}};
    AuthRateLimiter limiter{{
        .max_entries = 2U,
        .max_failures = 3U,
        .failure_window = std::chrono::seconds{10},
        .block_duration = std::chrono::seconds{20},
    }};

    EXPECT_TRUE(limiter.allowed("192.0.2.1", now));
    limiter.record_failure("192.0.2.1", now);
    limiter.record_failure("192.0.2.1", now + std::chrono::seconds{1});
    EXPECT_TRUE(limiter.allowed("192.0.2.1", now + std::chrono::seconds{2}));
    limiter.record_failure("192.0.2.1", now + std::chrono::seconds{2});
    EXPECT_FALSE(limiter.allowed("192.0.2.1", now + std::chrono::seconds{3}));
    EXPECT_TRUE(limiter.allowed("192.0.2.1", now + std::chrono::seconds{23}));
    limiter.record_success("192.0.2.1");
    EXPECT_EQ(limiter.size(), 0U);
}

} // namespace
} // namespace minitun::protocol
