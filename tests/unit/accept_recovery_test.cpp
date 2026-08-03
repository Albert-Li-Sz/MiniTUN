#include <cerrno>
#include <chrono>

#include <asio/error.hpp>
#include <asio/error_code.hpp>
#include <gtest/gtest.h>

#include <minitun/server/accept_recovery.hpp>

namespace minitun::server {
namespace {

TEST(AcceptRetryPolicyTest, AppliesBoundedExponentialBackoffAndResets) {
    using namespace std::chrono_literals;
    AcceptRetryPolicy policy;
    EXPECT_EQ(policy.next_delay(), 10ms);
    EXPECT_EQ(policy.next_delay(), 20ms);
    EXPECT_EQ(policy.next_delay(), 40ms);
    EXPECT_EQ(policy.next_delay(), 80ms);
    EXPECT_EQ(policy.next_delay(), 160ms);
    EXPECT_EQ(policy.next_delay(), 320ms);
    EXPECT_EQ(policy.next_delay(), 640ms);
    EXPECT_EQ(policy.next_delay(), 1s);
    EXPECT_EQ(policy.next_delay(), 1s);

    policy.reset();
    EXPECT_EQ(policy.next_delay(), 10ms);
}

TEST(AcceptRetryPolicyTest, RateLimitsRepeatedDiagnostics) {
    using namespace std::chrono_literals;
    AcceptRetryPolicy policy;
    const auto start = AcceptRetryPolicy::Clock::time_point{10s};
    EXPECT_TRUE(policy.should_log(start));
    EXPECT_FALSE(policy.should_log(start + 4999ms));
    EXPECT_TRUE(policy.should_log(start + 5s));
    policy.reset();
    EXPECT_TRUE(policy.should_log(start + 5001ms));
}

TEST(AcceptRetryPolicyTest, ClassifiesDescriptorAndBufferExhaustion) {
    const auto descriptor_error = asio::error::make_error_code(asio::error::no_descriptors);
    const auto system_descriptor_error = asio::error_code{ENFILE, asio::system_category()};
    const auto buffer_error = asio::error::make_error_code(asio::error::no_buffer_space);
    const auto aborted = asio::error::make_error_code(asio::error::operation_aborted);

    EXPECT_TRUE(AcceptRetryPolicy::descriptor_exhausted(descriptor_error));
    EXPECT_TRUE(AcceptRetryPolicy::descriptor_exhausted(system_descriptor_error));
    EXPECT_TRUE(AcceptRetryPolicy::resource_exhausted(buffer_error));
    EXPECT_TRUE(AcceptRetryPolicy::retryable(buffer_error));
    EXPECT_FALSE(AcceptRetryPolicy::retryable(aborted));
}

} // namespace
} // namespace minitun::server
