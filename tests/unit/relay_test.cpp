#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>
#include <minitun/protocol/relay.hpp>
#include <minitun/protocol/tls.hpp>

namespace minitun::protocol {
namespace {

asio::awaitable<void>
run_relay_with_invalid_timeout(TlsStream& tls_stream, asio::ip::tcp::socket& tcp_socket,
                               std::optional<common::Result<RelayStats>>& outcome) {
    outcome = co_await relay_tls_and_tcp(tls_stream, tcp_socket,
                                         {.inactivity_timeout = std::chrono::seconds::zero()});
}

TEST(RelayTest, UsesFixedBoundedDirectionBuffersAndZeroedStatistics) {
    EXPECT_EQ(kRelayBufferSize, 16U * 1024U);
    EXPECT_EQ(RelayStats{}, RelayStats{});
}

TEST(RelayTest, RejectsInvalidInactivityTimeoutBeforeUsingSockets) {
    asio::io_context io_context;
    asio::ssl::context tls_context{asio::ssl::context::tls_client};
    TlsStream tls_stream{io_context, tls_context};
    asio::ip::tcp::socket tcp_socket{io_context};
    std::optional<common::Result<RelayStats>> outcome;

    asio::co_spawn(io_context, run_relay_with_invalid_timeout(tls_stream, tcp_socket, outcome),
                   [](const std::exception_ptr failure) { EXPECT_FALSE(failure); });
    io_context.run();

    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error().code(), common::ErrorCode::invalid_argument);
}

} // namespace
} // namespace minitun::protocol
