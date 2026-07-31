#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <minitun/common/result.hpp>

namespace minitun::common {

using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;

/// Converts a system-clock time point to Unix time, flooring negative
/// fractional values toward negative infinity.
[[nodiscard]] std::int64_t to_unix_seconds(SystemClock::time_point time_point) noexcept;
[[nodiscard]] std::int64_t to_unix_milliseconds(SystemClock::time_point time_point) noexcept;

[[nodiscard]] std::int64_t unix_seconds_now() noexcept;
[[nodiscard]] std::int64_t unix_milliseconds_now() noexcept;

/// Monotonic time for elapsed-time measurement and timeout scheduling.
[[nodiscard]] SteadyClock::time_point steady_now() noexcept;

/// Computes |first - second| without signed overflow.
[[nodiscard]] std::uint64_t absolute_clock_skew_seconds(std::int64_t first,
                                                        std::int64_t second) noexcept;

/// Inclusively checks a timestamp against an allowed clock-skew window.
///
/// A negative allowed skew is a configuration error. Timestamp values may be
/// negative and the full int64 range is handled without signed overflow.
[[nodiscard]] Result<bool> is_clock_skew_within(std::int64_t timestamp, std::int64_t reference,
                                                std::chrono::seconds allowed_skew);

/// Formats a Unix timestamp as an ISO-8601 UTC value.
///
/// This uses the re-entrant POSIX UTC conversion API rather than std::gmtime.
[[nodiscard]] Result<std::string> format_unix_time_utc(std::int64_t timestamp);

} // namespace minitun::common
