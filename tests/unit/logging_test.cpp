#include <array>
#include <string>
#include <string_view>

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

TEST(StructuredLoggingTest, RoundTripsEveryLevelAndSupportsRuntimeThresholdChanges) {
    shutdown_logging();
    set_log_level(LogLevel::warn);
    EXPECT_FALSE(should_log(LogLevel::info));
    EXPECT_TRUE(should_log(LogLevel::error));
    EXPECT_FALSE(should_log(LogLevel::off));

    constexpr std::array levels{
        LogLevel::trace, LogLevel::debug,    LogLevel::info, LogLevel::warn,
        LogLevel::error, LogLevel::critical, LogLevel::off,
    };
    constexpr std::array<std::string_view, levels.size()> spellings{
        "trace", "debug", "info", "warn", "error", "critical", "off",
    };
    for (std::size_t index = 0; index < levels.size(); ++index) {
        EXPECT_EQ(to_string(levels[index]), spellings[index]);
        const auto parsed = log_level_from_string(spellings[index]);
        ASSERT_TRUE(parsed) << spellings[index];
        EXPECT_EQ(*parsed, levels[index]);
    }

    testing::internal::CaptureStdout();
    ASSERT_TRUE(initialize_logging(LoggingConfig{
        .logger_name = "runtime-level-test", .component = "unit-test", .level = LogLevel::trace}));
    for (const auto level : levels) {
        set_log_level(level);
    }
    set_log_level(LogLevel::info);
    EXPECT_FALSE(should_log(LogLevel::debug));
    EXPECT_TRUE(should_log(LogLevel::info));
    log_info("visible after runtime reconfiguration");
    shutdown_logging();
    EXPECT_FALSE(testing::internal::GetCapturedStdout().empty());

    constexpr auto invalid = static_cast<LogLevel>(255);
    EXPECT_EQ(to_string(invalid), "off");
    set_log_level(invalid);
    EXPECT_FALSE(should_log(LogLevel::critical));
    shutdown_logging();
}

TEST(StructuredLoggingTest, RejectsEmptyConfigurationFields) {
    const auto empty_name =
        initialize_logging(LoggingConfig{.logger_name = "", .component = "unit-test"});
    ASSERT_FALSE(empty_name);
    EXPECT_EQ(empty_name.error().code(), ErrorCode::invalid_argument);

    const auto empty_component =
        initialize_logging(LoggingConfig{.logger_name = "empty-component", .component = ""});
    ASSERT_FALSE(empty_component);
    EXPECT_EQ(empty_component.error().code(), ErrorCode::invalid_argument);
}

TEST(StructuredLoggingTest, EscapesEveryJsonControlCharacterAndUsesFallbackComponent) {
    testing::internal::CaptureStdout();
    ASSERT_TRUE(initialize_logging(LoggingConfig{
        .logger_name = "json-control-test", .component = "fallback", .level = LogLevel::trace}));

    std::string message{"quote=\" slash=\\ backspace="};
    message.push_back('\b');
    message.append(" formfeed=");
    message.push_back('\f');
    message.append(" newline=\n carriage=\r tab=\t control=");
    message.push_back('\x01');
    log_trace(message);
    shutdown_logging();

    const std::string output = testing::internal::GetCapturedStdout();
    ASSERT_TRUE(nlohmann::json::accept(output));
    const auto event = nlohmann::json::parse(output);
    EXPECT_EQ(event.at("component"), "fallback");
    EXPECT_EQ(event.at("message"), message);
    EXPECT_EQ(event.at("error_code"), "");
}

TEST(StructuredLoggingTest, AcceptsEveryCanonicalUtf8BoundarySequence) {
    std::string message{"ascii:"};
    constexpr std::array<std::string_view, 12> sequences{
        "\xc2\xa2",         "\xdf\xbf",         "\xe0\xa0\x80",     "\xed\x9f\xbf",
        "\xe1\x80\x80",     "\xec\xbf\xbf",     "\xee\x80\x80",     "\xef\xbf\xbf",
        "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf", "\xf1\x80\x80\x80", "\xf3\xbf\xbf\xbf",
    };
    for (const auto sequence : sequences) {
        message.append(sequence);
        message.push_back(':');
    }

    testing::internal::CaptureStdout();
    ASSERT_TRUE(initialize_logging(LoggingConfig{
        .logger_name = "utf8-boundary-test", .component = "unit-test", .level = LogLevel::info}));
    log_info(message);
    shutdown_logging();
    const auto event = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    EXPECT_EQ(event.at("message"), message);
}

TEST(StructuredLoggingTest, ReplacesEveryMalformedUtf8BoundaryClass) {
    std::string message{"invalid:"};
    constexpr std::array<std::string_view, 13> sequences{
        "\xc1\x80",
        "\xc2"
        "A",
        "\xe0\x9f\x80",
        "\xe0\xa0",
        "\xed\xa0\x80",
        "\xe1\x80"
        "A",
        "\xf0\x8f\x80\x80",
        "\xf4\x90\x80\x80",
        "\xf1\x80\x80"
        "A",
        "\xf0\x90"
        "A\x80",
        "\xf0\x90\x80"
        "A",
        "\xf0\x90\x80",
        "\xf5\x80\x80\x80",
    };
    for (const auto sequence : sequences) {
        message.append(sequence);
        message.push_back(':');
    }

    testing::internal::CaptureStdout();
    ASSERT_TRUE(initialize_logging(LoggingConfig{.logger_name = "utf8-invalid-class-test",
                                                 .component = "unit-test",
                                                 .level = LogLevel::info}));
    log_info(message);
    shutdown_logging();
    const std::string output = testing::internal::GetCapturedStdout();
    ASSERT_TRUE(nlohmann::json::accept(output));
    const auto sanitized =
        nlohmann::json::parse(output).at("message").get_ref<const std::string&>();
    EXPECT_NE(sanitized, message);
    EXPECT_NE(sanitized.find("\xef\xbf\xbd"), std::string::npos);
}

} // namespace
} // namespace minitun::common
