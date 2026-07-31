#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <minitun/common/error.hpp>
#include <minitun/common/logging.hpp>

namespace minitun::common {
namespace {

TEST(StructuredLoggingTest, EmitsOneValidJsonObjectWithEscapedFields) {
    testing::internal::CaptureStdout();

    const auto initialized = initialize_logging(LoggingConfig{
        .logger_name = "structured-logging-test",
        .component = "fallback-component",
        .level = LogLevel::trace,
    });
    ASSERT_TRUE(initialized);

    log_warn("line one\n\"quoted\"\tvalue", LogContext{
                                                .component = "control",
                                                .server_id = "srv_0123456789abcdef01234567",
                                                .tunnel_id = "tun_0123456789abcdef01234567",
                                                .connection_id = "conn_0123456789abcdef01234567",
                                                .remote_endpoint = "[2001:db8::1]:2333",
                                                .error_code = ErrorCode::connection_timeout,
                                            });
    shutdown_logging();

    const std::string output = testing::internal::GetCapturedStdout();
    ASSERT_FALSE(output.empty());

    const nlohmann::json event = nlohmann::json::parse(output);
    EXPECT_TRUE(event.at("timestamp").is_string());
    EXPECT_EQ(event.at("level"), "warning");
    EXPECT_EQ(event.at("component"), "control");
    EXPECT_EQ(event.at("server_id"), "srv_0123456789abcdef01234567");
    EXPECT_EQ(event.at("tunnel_id"), "tun_0123456789abcdef01234567");
    EXPECT_EQ(event.at("connection_id"), "conn_0123456789abcdef01234567");
    EXPECT_EQ(event.at("remote_endpoint"), "[2001:db8::1]:2333");
    EXPECT_EQ(event.at("error_code"), "connection_timeout");
    EXPECT_EQ(event.at("message"), "line one\n\"quoted\"\tvalue");
}

TEST(StructuredLoggingTest, HonorsOffLevelWithoutWriting) {
    testing::internal::CaptureStdout();

    const auto initialized = initialize_logging(LoggingConfig{
        .logger_name = "disabled-logging-test",
        .component = "unit-test",
        .level = LogLevel::off,
    });
    ASSERT_TRUE(initialized);

    log_error("this must not be emitted");
    shutdown_logging();

    EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
}

TEST(StructuredLoggingTest, RejectsOversizedConfigurationFields) {
    const auto oversized_name = initialize_logging(LoggingConfig{
        .logger_name = std::string(kMaxLoggerNameBytes + 1U, 'n'), .component = "unit-test"});
    ASSERT_FALSE(oversized_name);
    EXPECT_EQ(oversized_name.error().code(), ErrorCode::invalid_argument);

    const auto oversized_component = initialize_logging(
        LoggingConfig{.logger_name = "bounded-logging-test",
                      .component = std::string(kMaxLogComponentBytes + 1U, 'c')});
    ASSERT_FALSE(oversized_component);
    EXPECT_EQ(oversized_component.error().code(), ErrorCode::invalid_argument);
}

TEST(StructuredLoggingTest, TruncatesOversizedEventFieldsToPublicLimits) {
    testing::internal::CaptureStdout();

    const auto initialized = initialize_logging(LoggingConfig{
        .logger_name = "truncated-logging-test",
        .component = "unit-test",
        .level = LogLevel::warn,
    });
    ASSERT_TRUE(initialized);

    const std::string component(kMaxLogComponentBytes + 20U, 'c');
    const std::string identifier(kMaxLogIdentifierBytes + 20U, 'i');
    const std::string endpoint(kMaxLogEndpointBytes + 20U, 'e');
    const std::string message(kMaxLogMessageBytes + 20U, 'm');
    log_warn(message, LogContext{
                          .component = component,
                          .server_id = identifier,
                          .remote_endpoint = endpoint,
                      });
    shutdown_logging();

    const nlohmann::json event = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    EXPECT_EQ(event.at("component").get_ref<const std::string&>().size(), kMaxLogComponentBytes);
    EXPECT_EQ(event.at("server_id").get_ref<const std::string&>().size(), kMaxLogIdentifierBytes);
    EXPECT_EQ(event.at("remote_endpoint").get_ref<const std::string&>().size(),
              kMaxLogEndpointBytes);
    EXPECT_EQ(event.at("message").get_ref<const std::string&>().size(), kMaxLogMessageBytes);
}

TEST(StructuredLoggingTest, DoesNotSplitUtf8CodePointsAtTheMessageLimit) {
    testing::internal::CaptureStdout();

    const auto initialized = initialize_logging(LoggingConfig{
        .logger_name = "utf8-logging-test",
        .component = "unit-test",
        .level = LogLevel::warn,
    });
    ASSERT_TRUE(initialized);

    std::string message(kMaxLogMessageBytes - 1U, 'a');
    message.append("\xF0\x9F\x98\x80");
    log_warn(message);
    shutdown_logging();

    const nlohmann::json event = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    EXPECT_EQ(event.at("message").get_ref<const std::string&>().size(), kMaxLogMessageBytes - 1U);
}

TEST(StructuredLoggingTest, ReplacesMalformedUtf8BeforeJsonEncoding) {
    testing::internal::CaptureStdout();

    const auto initialized = initialize_logging(LoggingConfig{
        .logger_name = "invalid-utf8-logging-test",
        .component = "unit-test",
        .level = LogLevel::warn,
    });
    ASSERT_TRUE(initialized);

    std::string message{"before"};
    message.push_back(static_cast<char>(0xffU));
    message.append("middle");
    message.push_back(static_cast<char>(0x80U));
    message.append("after");
    message.push_back(static_cast<char>(0xc2U));
    log_warn(message);
    shutdown_logging();

    const std::string output = testing::internal::GetCapturedStdout();
    ASSERT_TRUE(nlohmann::json::accept(output));

    std::string expected{"before"};
    expected.append("\xef\xbf\xbd");
    expected.append("middle");
    expected.append("\xef\xbf\xbd");
    expected.append("after");
    expected.append("\xef\xbf\xbd");
    const nlohmann::json event = nlohmann::json::parse(output);
    EXPECT_EQ(event.at("message"), expected);
}

TEST(StructuredLoggingTest, KeepsReplacementWithinTheConfiguredByteLimit) {
    testing::internal::CaptureStdout();

    const auto initialized = initialize_logging(LoggingConfig{
        .logger_name = "bounded-invalid-utf8-test",
        .component = "unit-test",
        .level = LogLevel::warn,
    });
    ASSERT_TRUE(initialized);

    std::string message(kMaxLogMessageBytes - 2U, 'a');
    message.push_back(static_cast<char>(0xffU));
    log_warn(message);
    shutdown_logging();

    const nlohmann::json event = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    EXPECT_EQ(event.at("message").get_ref<const std::string&>().size(), kMaxLogMessageBytes - 2U);
}

} // namespace
} // namespace minitun::common
