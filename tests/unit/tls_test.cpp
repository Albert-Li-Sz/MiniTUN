#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>
#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>
#include <minitun/protocol/frame.hpp>
#include <minitun/protocol/tls.hpp>

namespace minitun::protocol {
namespace {

asio::awaitable<void> read_one(TlsStream& stream, std::optional<common::Result<Frame>>& outcome) {
    outcome = co_await async_read_frame(stream);
}

asio::awaitable<void> write_one(TlsStream& stream, Frame frame,
                                std::optional<common::Result<void>>& outcome) {
    outcome = co_await async_write_frame(stream, std::move(frame));
}

asio::awaitable<void> write_many(TlsStream& stream, std::vector<Frame> frames,
                                 std::optional<common::Result<void>>& outcome) {
    outcome = co_await async_write_frames(stream, std::move(frames));
}

template <typename Awaitable> void run(asio::io_context& io_context, Awaitable awaitable) {
    asio::co_spawn(io_context, std::move(awaitable),
                   [](const std::exception_ptr& failure) { EXPECT_FALSE(failure); });
    io_context.run();
    io_context.restart();
}

[[nodiscard]] Frame ping_frame(const std::uint32_t flags = 0U) {
    Frame frame;
    frame.type = MessageType::ping;
    frame.flags = flags;
    return frame;
}

TEST(TlsTest, RejectsEveryMalformedServerContextPath) {
    const auto empty_certificate = make_server_tls_context({});
    ASSERT_FALSE(empty_certificate);
    EXPECT_EQ(empty_certificate.error().code(), common::ErrorCode::invalid_argument);

    const auto empty_key = make_server_tls_context({.certificate_chain_path = "certificate.pem",
                                                    .private_key_path = "",
                                                    .client_ca_certificate_path = ""});
    ASSERT_FALSE(empty_key);
    EXPECT_EQ(empty_key.error().code(), common::ErrorCode::invalid_argument);

    const std::string oversized(4'097U, 'x');
    const std::string nul_path{"bad\0path", 8U};
    for (const auto& certificate : {oversized, nul_path}) {
        const auto result = make_server_tls_context({.certificate_chain_path = certificate,
                                                     .private_key_path = "key.pem",
                                                     .client_ca_certificate_path = ""});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    }
    for (const auto& key : {oversized, nul_path}) {
        const auto result = make_server_tls_context({.certificate_chain_path = "certificate.pem",
                                                     .private_key_path = key,
                                                     .client_ca_certificate_path = ""});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    }
    for (const auto& ca : {oversized, nul_path}) {
        const auto result = make_server_tls_context({.certificate_chain_path = "certificate.pem",
                                                     .private_key_path = "key.pem",
                                                     .client_ca_certificate_path = ca});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    }

    const auto missing_files =
        make_server_tls_context({.certificate_chain_path = "/does/not/exist/certificate.pem",
                                 .private_key_path = "/does/not/exist/key.pem",
                                 .client_ca_certificate_path = ""});
    ASSERT_FALSE(missing_files);
    EXPECT_EQ(missing_files.error().code(), common::ErrorCode::tls_error);
}

TEST(TlsTest, ValidatesClientContextSourceCombinationsAndContent) {
    const auto conflicting_ca =
        make_client_tls_context({.ca_certificate_path = "ca.pem", .ca_certificate_pem = "inline"});
    ASSERT_FALSE(conflicting_ca);
    EXPECT_EQ(conflicting_ca.error().code(), common::ErrorCode::invalid_argument);

    const auto certificate_only =
        make_client_tls_context({.client_certificate_pem = "certificate"});
    ASSERT_FALSE(certificate_only);
    EXPECT_EQ(certificate_only.error().code(), common::ErrorCode::invalid_argument);
    const auto key_only = make_client_tls_context({.client_private_key_pem = "key"});
    ASSERT_FALSE(key_only);
    EXPECT_EQ(key_only.error().code(), common::ErrorCode::invalid_argument);

    const std::string oversized(4'097U, 'x');
    const std::string nul_path{"bad\0path", 8U};
    for (const auto& path : {oversized, nul_path}) {
        const auto result = make_client_tls_context({.ca_certificate_path = path});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    }

    const auto invalid_inline = make_client_tls_context({.ca_certificate_pem = "not a CA"});
    ASSERT_FALSE(invalid_inline);
    EXPECT_EQ(invalid_inline.error().code(), common::ErrorCode::tls_error);
    const auto missing_path =
        make_client_tls_context({.ca_certificate_path = "/does/not/exist/ca.pem"});
    ASSERT_FALSE(missing_path);
    EXPECT_EQ(missing_path.error().code(), common::ErrorCode::tls_error);

    const auto system_roots = make_client_tls_context({});
    ASSERT_TRUE(system_roots) << system_roots.error();
}

TEST(TlsTest, ConfiguresClientVerificationAndRejectsInvalidServerNames) {
    asio::io_context io_context;
    asio::ssl::context context{asio::ssl::context::tls_client};

    for (const auto& server_name :
         {std::string{}, std::string(254U, 'a'), std::string{"bad\0name", 8U}}) {
        TlsStream stream{io_context, context};
        const auto result = configure_client_tls_stream(stream, server_name, false);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    }

    TlsStream verified{io_context, context};
    EXPECT_TRUE(configure_client_tls_stream(verified, "example.test", false));
    TlsStream insecure{io_context, context};
    EXPECT_TRUE(configure_client_tls_stream(insecure, "example.test", true));
}

TEST(TlsTest, EmptySessionCacheAndTransportHelpersAreSafe) {
    asio::io_context io_context;
    asio::ssl::context context{asio::ssl::context::tls_client};
    TlsStream stream{io_context, context};
    TlsSessionCache cache;

    EXPECT_FALSE(cache.restore(stream));
    EXPECT_FALSE(cache.capture(stream));
    EXPECT_FALSE(tls_session_reused(stream));
    configure_tcp_transport(stream.lowest_layer());
    close_tls_stream(stream);
    close_tls_stream(stream);
}

TEST(TlsTest, ReportsPreflightAndDisconnectedFrameIoFailures) {
    asio::io_context io_context;
    asio::ssl::context context{asio::ssl::context::tls_client};
    TlsStream stream{io_context, context};
    std::optional<common::Result<void>> write_outcome;

    Frame invalid = ping_frame(1U);
    run(io_context, write_one(stream, invalid, write_outcome));
    ASSERT_TRUE(write_outcome.has_value());
    ASSERT_FALSE(*write_outcome);
    EXPECT_EQ(write_outcome->error().code(), common::ErrorCode::protocol_error);

    write_outcome.reset();
    run(io_context, write_one(stream, ping_frame(), write_outcome));
    ASSERT_TRUE(write_outcome.has_value());
    ASSERT_FALSE(*write_outcome);
    EXPECT_EQ(write_outcome->error().code(), common::ErrorCode::connection_failed);

    std::optional<common::Result<Frame>> read_outcome;
    run(io_context, read_one(stream, read_outcome));
    ASSERT_TRUE(read_outcome.has_value());
    ASSERT_FALSE(*read_outcome);
    EXPECT_EQ(read_outcome->error().code(), common::ErrorCode::connection_failed);
}

TEST(TlsTest, BoundsPipelinedWritesBeforeTouchingTransport) {
    asio::io_context io_context;
    asio::ssl::context context{asio::ssl::context::tls_client};
    TlsStream stream{io_context, context};
    std::optional<common::Result<void>> outcome;

    run(io_context, write_many(stream, {}, outcome));
    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error().code(), common::ErrorCode::invalid_argument);

    outcome.reset();
    run(io_context, write_many(stream, std::vector<Frame>(33U, ping_frame()), outcome));
    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error().code(), common::ErrorCode::invalid_argument);

    outcome.reset();
    run(io_context, write_many(stream, {ping_frame(1U)}, outcome));
    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error().code(), common::ErrorCode::protocol_error);

    outcome.reset();
    run(io_context, write_many(stream, {ping_frame()}, outcome));
    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error().code(), common::ErrorCode::connection_failed);
}

} // namespace
} // namespace minitun::protocol
