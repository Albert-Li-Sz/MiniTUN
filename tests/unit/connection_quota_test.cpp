#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
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

} // namespace
} // namespace minitun::server
