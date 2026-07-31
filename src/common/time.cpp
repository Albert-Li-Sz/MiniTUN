#include <minitun/common/time.hpp>

#include <array>
#include <ctime>
#include <limits>
#include <type_traits>

namespace minitun::common {
namespace {

[[nodiscard]] bool fits_time_t(const std::int64_t timestamp) noexcept {
    if constexpr (std::numeric_limits<std::time_t>::is_signed) {
        if constexpr (std::numeric_limits<std::time_t>::digits >=
                      std::numeric_limits<std::int64_t>::digits) {
            return true;
        } else {
            return timestamp >=
                       static_cast<std::int64_t>(std::numeric_limits<std::time_t>::min()) &&
                   timestamp <= static_cast<std::int64_t>(std::numeric_limits<std::time_t>::max());
        }
    } else {
        if (timestamp < 0) {
            return false;
        }
        if constexpr (std::numeric_limits<std::time_t>::digits >=
                      std::numeric_limits<std::uint64_t>::digits) {
            return true;
        } else {
            return static_cast<std::uint64_t>(timestamp) <=
                   static_cast<std::uint64_t>(std::numeric_limits<std::time_t>::max());
        }
    }
}

} // namespace

std::int64_t to_unix_seconds(const SystemClock::time_point time_point) noexcept {
    return std::chrono::floor<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

std::int64_t to_unix_milliseconds(const SystemClock::time_point time_point) noexcept {
    return std::chrono::floor<std::chrono::milliseconds>(time_point.time_since_epoch()).count();
}

std::int64_t unix_seconds_now() noexcept { return to_unix_seconds(SystemClock::now()); }

std::int64_t unix_milliseconds_now() noexcept { return to_unix_milliseconds(SystemClock::now()); }

SteadyClock::time_point steady_now() noexcept { return SteadyClock::now(); }

std::uint64_t absolute_clock_skew_seconds(const std::int64_t first,
                                          const std::int64_t second) noexcept {
    if (first >= second) {
        return static_cast<std::uint64_t>(first) - static_cast<std::uint64_t>(second);
    }
    return static_cast<std::uint64_t>(second) - static_cast<std::uint64_t>(first);
}

Result<bool> is_clock_skew_within(const std::int64_t timestamp, const std::int64_t reference,
                                  const std::chrono::seconds allowed_skew) {
    if (allowed_skew.count() < 0) {
        return Result<bool>::failure(ErrorCode::invalid_argument,
                                     "allowed clock skew must not be negative");
    }

    return absolute_clock_skew_seconds(timestamp, reference) <=
           static_cast<std::uint64_t>(allowed_skew.count());
}

Result<std::string> format_unix_time_utc(const std::int64_t timestamp) {
    if (!fits_time_t(timestamp)) {
        return Result<std::string>::failure(ErrorCode::invalid_argument,
                                            "Unix timestamp is outside the platform range");
    }

    const std::time_t raw_time = static_cast<std::time_t>(timestamp);
    std::tm utc_time{};
    if (::gmtime_r(&raw_time, &utc_time) == nullptr) {
        return Result<std::string>::failure(ErrorCode::invalid_argument,
                                            "Unix timestamp cannot be represented as UTC");
    }

    std::array<char, 32> formatted{};
    if (std::strftime(formatted.data(), formatted.size(), "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
        return Result<std::string>::failure(ErrorCode::internal_error,
                                            "UTC timestamp formatting failed");
    }

    return std::string{formatted.data()};
}

} // namespace minitun::common
