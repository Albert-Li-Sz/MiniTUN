#include <string>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/server/session_registry.hpp>

namespace minitun::server {
namespace {

[[nodiscard]] std::string client_id() {
    auto id = common::Id::generate(common::IdKind::client);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

TEST(ServerSessionRegistryTest, ReconnectReplacesGenerationAndOldCloseCannotEraseIt) {
    SessionRegistry registry{2U};
    const std::string id = client_id();
    const auto first = registry.open(id);
    const auto second = registry.open(id);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
    EXPECT_FALSE(registry.is_current(id, *first));
    EXPECT_TRUE(registry.is_current(id, *second));

    registry.close(id, *first);
    EXPECT_TRUE(registry.is_current(id, *second));
    registry.close(id, *second);
    EXPECT_EQ(registry.size(), 0U);
}

TEST(ServerSessionRegistryTest, EnforcesDistinctClientLimit) {
    SessionRegistry registry{1U};
    const auto first = registry.open(client_id());
    ASSERT_TRUE(first);
    const auto second = registry.open(client_id());
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code(), common::ErrorCode::resource_exhausted);
}

TEST(ServerSessionRegistryTest, RejectsMalformedClientId) {
    SessionRegistry registry{1U};
    const auto invalid = registry.open("not-a-client");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), common::ErrorCode::invalid_argument);
}

} // namespace
} // namespace minitun::server
