#include <chrono>
#include <string>
#include <utility>

#include <asio/ip/address.hpp>
#include <asio/ip/address_v4.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/server/client_policy.hpp>
#include <minitun/server/connection_quota.hpp>

namespace minitun::server {
namespace {

[[nodiscard]] std::string generated_client_id() {
    auto id = common::Id::generate(common::IdKind::client);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

TEST(ConnectionQuotaTest, EnforcesPerClientAndGlobalLimits) {
    ConnectionQuota quota{2U, 3U};
    const std::string first_client = generated_client_id();
    const std::string second_client = generated_client_id();

    auto first = quota.try_acquire(first_client);
    auto second = quota.try_acquire(first_client);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto per_client = quota.try_acquire(first_client);
    ASSERT_FALSE(per_client);
    EXPECT_EQ(per_client.error().code(), common::ErrorCode::resource_exhausted);

    auto third = quota.try_acquire(second_client);
    ASSERT_TRUE(third);
    const auto global = quota.try_acquire(second_client);
    ASSERT_FALSE(global);
    EXPECT_EQ(global.error().code(), common::ErrorCode::resource_exhausted);
    EXPECT_EQ(quota.total_in_use(), 3U);
    EXPECT_EQ(quota.client_in_use(first_client), 2U);
}

TEST(ConnectionQuotaTest, MoveTransfersOwnershipAndReleaseRestoresCapacity) {
    ConnectionQuota quota{1U, 1U};
    const std::string client_id = generated_client_id();
    auto acquired = quota.try_acquire(client_id);
    ASSERT_TRUE(acquired);

    ConnectionQuota::Lease lease = std::move(*acquired);
    EXPECT_TRUE(static_cast<bool>(lease));
    EXPECT_FALSE(static_cast<bool>(*acquired));
    lease = {};
    EXPECT_EQ(quota.total_in_use(), 0U);
    EXPECT_TRUE(quota.try_acquire(client_id));
}

TEST(ConnectionQuotaTest, RejectsMalformedClientIdentity) {
    ConnectionQuota quota{1U, 1U};
    const auto acquired = quota.try_acquire("not-a-client");
    ASSERT_FALSE(acquired);
    EXPECT_EQ(acquired.error().code(), common::ErrorCode::invalid_argument);
}

TEST(ConnectionQuotaTest, RejectsInvalidPerClientOverridesAndReportsUnknownClients) {
    ConnectionQuota quota{2U, 4U};
    const std::string client = generated_client_id();

    const auto zero = quota.try_acquire(client, 0U);
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().code(), common::ErrorCode::invalid_argument);
    const auto oversized = quota.try_acquire(client, 3U);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code(), common::ErrorCode::invalid_argument);

    EXPECT_EQ(quota.client_in_use(client), 0U);
    EXPECT_EQ(quota.client_in_use("client_00000000000000000000000000000000"), 0U);
}

TEST(ConnectionQuotaTest, SourceLimiterAdmitsUpToRateAndRefillsOverTime) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{std::chrono::seconds{100}};
    const auto client = generated_client_id();
    const auto source = asio::ip::make_address("198.51.100.7");
    SourceConnectionLimiter limiter;

    EXPECT_TRUE(limiter.allow(client, source, 2U, start));
    EXPECT_TRUE(limiter.allow(client, source, 2U, start));
    EXPECT_FALSE(limiter.allow(client, source, 2U, start));
    EXPECT_TRUE(limiter.allow(client, source, 2U, start + std::chrono::minutes{1}));
    EXPECT_TRUE(limiter.allow(client, source, 2U, start + std::chrono::minutes{1}));
    EXPECT_FALSE(limiter.allow(client, source, 2U, start + std::chrono::minutes{1}));

    // Zero rate admits nothing; distinct clients and sources are isolated.
    EXPECT_FALSE(limiter.allow(client, source, 0U, start));
    EXPECT_TRUE(limiter.allow(generated_client_id(), source, 1U, start));
    EXPECT_TRUE(limiter.allow(client, asio::ip::make_address("192.0.2.9"), 1U, start));
    EXPECT_FALSE(limiter.allow(client, source, 1U, start + std::chrono::seconds{30}));
}

TEST(ConnectionQuotaTest, SourceLimiterStaysBoundedUnderManyDistinctSources) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{std::chrono::seconds{100}};
    const auto client = generated_client_id();
    SourceConnectionLimiter limiter;
    for (std::uint32_t index = 0U; index < 5'000U; ++index) {
        const auto address =
            asio::ip::address_v4{0xcb000001U + index}.to_string();
        static_cast<void>(limiter.allow(client, asio::ip::make_address(address), 1U, start));
    }
}

} // namespace
} // namespace minitun::server
