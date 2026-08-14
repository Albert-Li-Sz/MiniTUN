#include <array>
#include <cerrno>
#include <chrono>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/error_code.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include <fcntl.h>

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
    EXPECT_TRUE(AcceptRetryPolicy::resource_exhausted(asio::error::no_memory));
    EXPECT_FALSE(AcceptRetryPolicy::resource_exhausted(asio::error::fault));
    EXPECT_TRUE(AcceptRetryPolicy::retryable(buffer_error));
    EXPECT_FALSE(AcceptRetryPolicy::retryable(aborted));
    EXPECT_FALSE(AcceptRetryPolicy::retryable(asio::error_code{}));
    EXPECT_FALSE(AcceptRetryPolicy::retryable(asio::error::bad_descriptor));
    EXPECT_TRUE(AcceptRetryPolicy::retryable(asio::error::connection_reset));
    EXPECT_FALSE(AcceptRetryPolicy::descriptor_exhausted(asio::error::fault));
}

TEST(ReservedFileDescriptorTest, ClosesReopensAndDropsOneQueuedConnection) {
    ReservedFileDescriptor reserve;
    EXPECT_TRUE(reserve.available());
    reserve.reopen();
    EXPECT_TRUE(reserve.available());
    reserve.close();
    EXPECT_FALSE(reserve.available());
    reserve.close();
    reserve.reopen();
    EXPECT_TRUE(reserve.available());

    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor{
        io_context, asio::ip::tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0U}};
    asio::ip::tcp::socket client{io_context};
    client.connect(acceptor.local_endpoint());
    reserve.recover(acceptor);
    EXPECT_TRUE(reserve.available());

    std::array<char, 1U> byte{};
    asio::error_code error;
    EXPECT_EQ(client.read_some(asio::buffer(byte), error), 0U);
    EXPECT_EQ(error, asio::error::eof);
}

TEST(ReservedFileDescriptorTest, RecoversWithoutBlockingWhenNoConnectionIsQueued) {
    ReservedFileDescriptor reserve;
    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor{
        io_context, asio::ip::tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0U}};
    const int descriptor = acceptor.native_handle();
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK), 0);

    reserve.recover(acceptor);
    EXPECT_TRUE(reserve.available());
}

} // namespace
} // namespace minitun::server
