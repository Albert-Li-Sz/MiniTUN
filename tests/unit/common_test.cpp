#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/result.hpp>
#include <minitun/common/version.hpp>

namespace minitun::common {
namespace {

TEST(ErrorCodeTest, HasStableRoundTripSpelling) {
    constexpr std::array codes{
        ErrorCode::ok,
        ErrorCode::invalid_argument,
        ErrorCode::not_found,
        ErrorCode::already_exists,
        ErrorCode::permission_denied,
        ErrorCode::not_authenticated,
        ErrorCode::authentication_failed,
        ErrorCode::connection_failed,
        ErrorCode::connection_timeout,
        ErrorCode::remote_port_in_use,
        ErrorCode::local_connect_failed,
        ErrorCode::protocol_error,
        ErrorCode::frame_too_large,
        ErrorCode::unsupported_version,
        ErrorCode::resource_exhausted,
        ErrorCode::database_error,
        ErrorCode::tls_error,
        ErrorCode::ipc_error,
        ErrorCode::internal_error,
    };

    for (const ErrorCode code : codes) {
        const auto parsed = error_code_from_string(to_string(code));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, code);
    }
    EXPECT_FALSE(error_code_from_string("not_a_real_error").has_value());
}

TEST(ErrorTest, AddsNonSensitiveContext) {
    const Error error{ErrorCode::connection_failed, "connection refused"};

    const Error contextual = error.with_context("public server");

    EXPECT_EQ(contextual.code(), ErrorCode::connection_failed);
    EXPECT_EQ(contextual.message(), "public server: connection refused");
}

TEST(ResultTest, RepresentsValueAndFailureWithoutExceptionsAtBoundary) {
    Result<std::string> value = std::string{"online"};
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, "online");

    Result<std::string> failure =
        Error{ErrorCode::connection_timeout, "control connection timed out"};
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().code(), ErrorCode::connection_timeout);

    const Result<void> success = Result<void>::success();
    EXPECT_TRUE(success);
}

TEST(LoggingTest, ValidatesLevelsAndConfiguration) {
    const auto debug_level = log_level_from_string("debug");
    ASSERT_TRUE(debug_level);
    EXPECT_EQ(*debug_level, LogLevel::debug);

    const auto invalid_level = log_level_from_string("verbose");
    ASSERT_FALSE(invalid_level);
    EXPECT_EQ(invalid_level.error().code(), ErrorCode::invalid_argument);

    const auto initialized = initialize_logging(LoggingConfig{
        .logger_name = "minitun-test", .component = "unit-test", .level = LogLevel::off});
    ASSERT_TRUE(initialized);
    EXPECT_FALSE(should_log(LogLevel::critical));
    shutdown_logging();
}

TEST(VersionTest, ReportsEveryRequiredBuildField) {
    const VersionInfo info = version_info();

    EXPECT_FALSE(info.version.empty());
    EXPECT_FALSE(info.git_commit.empty());
    EXPECT_FALSE(info.build_type.empty());
    EXPECT_FALSE(info.compiler.empty());
    EXPECT_EQ(info.protocol_version, std::uint16_t{2});

    const std::string formatted = format_version_info("minitun-test");
    EXPECT_NE(formatted.find("minitun-test "), std::string::npos);
    EXPECT_NE(formatted.find("git commit: "), std::string::npos);
    EXPECT_NE(formatted.find("build type: "), std::string::npos);
    EXPECT_NE(formatted.find("compiler: "), std::string::npos);
    EXPECT_NE(formatted.find("protocol version: 2"), std::string::npos);
}

} // namespace
} // namespace minitun::common
