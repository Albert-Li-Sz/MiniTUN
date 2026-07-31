#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>

namespace minitun::common {

enum class LogLevel {
    trace,
    debug,
    info,
    warn,
    error,
    critical,
    off,
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;
[[nodiscard]] Result<LogLevel> log_level_from_string(std::string_view value);

struct LoggingConfig final {
    std::string logger_name{"minitun"};
    std::string component{"minitun"};
    LogLevel level{LogLevel::info};
};

/// Per-event correlation fields. The logging implementation only accepts this
/// allowlist so credentials cannot accidentally be attached as arbitrary
/// structured fields.
struct LogContext final {
    std::string_view component{};
    std::string_view server_id{};
    std::string_view tunnel_id{};
    std::string_view connection_id{};
    std::string_view remote_endpoint{};
    std::optional<ErrorCode> error_code{};
};

/// Installs a thread-safe stdout logger suitable for journald collection.
///
/// Each event is one JSON object. Calling this function again atomically
/// replaces the process logger and its default component.
[[nodiscard]] Result<void> initialize_logging(const LoggingConfig& config = {});

void shutdown_logging() noexcept;
void set_log_level(LogLevel level) noexcept;
[[nodiscard]] bool should_log(LogLevel level) noexcept;

/// Emits a structured event. Logging failures are contained and never escape
/// into network or shutdown paths.
void log(LogLevel level, std::string_view message, const LogContext& context = {}) noexcept;

inline void log_trace(std::string_view message, const LogContext& context = {}) noexcept {
    log(LogLevel::trace, message, context);
}

inline void log_debug(std::string_view message, const LogContext& context = {}) noexcept {
    log(LogLevel::debug, message, context);
}

inline void log_info(std::string_view message, const LogContext& context = {}) noexcept {
    log(LogLevel::info, message, context);
}

inline void log_warn(std::string_view message, const LogContext& context = {}) noexcept {
    log(LogLevel::warn, message, context);
}

inline void log_error(std::string_view message, const LogContext& context = {}) noexcept {
    log(LogLevel::error, message, context);
}

} // namespace minitun::common
