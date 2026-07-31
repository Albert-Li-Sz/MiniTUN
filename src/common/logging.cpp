#include <minitun/common/logging.hpp>

#include <array>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

namespace minitun::common {
namespace {

constexpr std::string_view kJsonPattern =
    R"({"timestamp":"%Y-%m-%dT%H:%M:%S.%e%z","level":"%l",%v})";

std::mutex logger_mutex;
std::shared_ptr<spdlog::logger> process_logger;
std::string default_component{"minitun"};
LogLevel configured_level{LogLevel::info};

spdlog::level::level_enum to_spdlog_level(const LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace:
        return spdlog::level::trace;
    case LogLevel::debug:
        return spdlog::level::debug;
    case LogLevel::info:
        return spdlog::level::info;
    case LogLevel::warn:
        return spdlog::level::warn;
    case LogLevel::error:
        return spdlog::level::err;
    case LogLevel::critical:
        return spdlog::level::critical;
    case LogLevel::off:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

void append_json_string(std::string& output, const std::string_view value) {
    constexpr std::array<char, 16> hex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    output.push_back('"');
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"':
            output.append(R"(\")");
            break;
        case '\\':
            output.append(R"(\\)");
            break;
        case '\b':
            output.append(R"(\b)");
            break;
        case '\f':
            output.append(R"(\f)");
            break;
        case '\n':
            output.append(R"(\n)");
            break;
        case '\r':
            output.append(R"(\r)");
            break;
        case '\t':
            output.append(R"(\t)");
            break;
        default:
            if (character < 0x20U) {
                output.append(R"(\u00)");
                output.push_back(hex[(character >> 4U) & 0x0fU]);
                output.push_back(hex[character & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_json_field(std::string& output, const std::string_view key,
                       const std::string_view value, const bool append_comma = true) {
    output.push_back('"');
    output.append(key);
    output.append(R"(":)");
    append_json_string(output, value);
    if (append_comma) {
        output.push_back(',');
    }
}

std::string make_payload(const std::string_view fallback_component, const std::string_view message,
                         const LogContext& context) {
    std::string payload;
    payload.reserve(fallback_component.size() + message.size() + context.component.size() +
                    context.server_id.size() + context.tunnel_id.size() +
                    context.connection_id.size() + context.remote_endpoint.size() + 160U);

    const std::string_view component =
        context.component.empty() ? fallback_component : context.component;
    append_json_field(payload, "component", component);
    append_json_field(payload, "server_id", context.server_id);
    append_json_field(payload, "tunnel_id", context.tunnel_id);
    append_json_field(payload, "connection_id", context.connection_id);
    append_json_field(payload, "remote_endpoint", context.remote_endpoint);
    append_json_field(payload, "error_code",
                      context.error_code.has_value() ? to_string(*context.error_code)
                                                     : std::string_view{});
    append_json_field(payload, "message", message, false);
    return payload;
}

} // namespace

std::string_view to_string(const LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace:
        return "trace";
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warn:
        return "warn";
    case LogLevel::error:
        return "error";
    case LogLevel::critical:
        return "critical";
    case LogLevel::off:
        return "off";
    }
    return "off";
}

Result<LogLevel> log_level_from_string(const std::string_view value) {
    if (value == "trace") {
        return LogLevel::trace;
    }
    if (value == "debug") {
        return LogLevel::debug;
    }
    if (value == "info") {
        return LogLevel::info;
    }
    if (value == "warn") {
        return LogLevel::warn;
    }
    if (value == "error") {
        return LogLevel::error;
    }
    if (value == "critical") {
        return LogLevel::critical;
    }
    if (value == "off") {
        return LogLevel::off;
    }

    return Error{
        ErrorCode::invalid_argument,
        "invalid log level; expected trace, debug, info, warn, error, critical, or off",
    };
}

Result<void> initialize_logging(const LoggingConfig& config) {
    if (config.logger_name.empty()) {
        return Error{ErrorCode::invalid_argument, "logger name must not be empty"};
    }
    if (config.component.empty()) {
        return Error{ErrorCode::invalid_argument, "default log component must not be empty"};
    }

    try {
        auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
        auto new_logger = std::make_shared<spdlog::logger>(config.logger_name, std::move(sink));
        new_logger->set_pattern(std::string{kJsonPattern});
        new_logger->set_level(to_spdlog_level(config.level));
        new_logger->flush_on(spdlog::level::warn);

        std::scoped_lock lock{logger_mutex};
        process_logger = std::move(new_logger);
        default_component = config.component;
        configured_level = config.level;
        return Result<void>::success();
    } catch (const spdlog::spdlog_ex& exception) {
        return Error{ErrorCode::internal_error,
                     std::string{"failed to initialize logging: "} + exception.what()};
    } catch (const std::exception& exception) {
        return Error{ErrorCode::internal_error,
                     std::string{"failed to initialize logging: "} + exception.what()};
    }
}

void shutdown_logging() noexcept {
    std::shared_ptr<spdlog::logger> old_logger;
    {
        std::scoped_lock lock{logger_mutex};
        old_logger = std::exchange(process_logger, {});
    }

    if (old_logger != nullptr) {
        try {
            old_logger->flush();
        } catch (...) {
            // Logging must not destabilize shutdown.
        }
    }
}

void set_log_level(const LogLevel level) noexcept {
    try {
        std::scoped_lock lock{logger_mutex};
        configured_level = level;
        if (process_logger != nullptr) {
            process_logger->set_level(to_spdlog_level(level));
        }
    } catch (...) {
        // Logging reconfiguration must not escape into the caller.
    }
}

bool should_log(const LogLevel level) noexcept {
    try {
        std::scoped_lock lock{logger_mutex};
        if (process_logger != nullptr) {
            return process_logger->should_log(to_spdlog_level(level));
        }
        return level != LogLevel::off &&
               to_spdlog_level(level) >= to_spdlog_level(configured_level);
    } catch (...) {
        return false;
    }
}

void log(const LogLevel level, const std::string_view message, const LogContext& context) noexcept {
    try {
        std::shared_ptr<spdlog::logger> logger;
        std::string component;
        {
            std::scoped_lock lock{logger_mutex};
            logger = process_logger;
            component = default_component;
        }

        if (logger == nullptr || !logger->should_log(to_spdlog_level(level))) {
            return;
        }

        const std::string payload = make_payload(component, message, context);
        logger->log(to_spdlog_level(level), spdlog::string_view_t{payload.data(), payload.size()});
    } catch (...) {
        // An unavailable sink must never terminate a daemon or network callback.
    }
}

} // namespace minitun::common
