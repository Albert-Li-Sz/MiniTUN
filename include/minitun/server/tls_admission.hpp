#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <minitun/protocol/auth.hpp>

namespace minitun::server {

// All calls belong to the listener strand. Admission precedes SSL allocation;
// reservations last until application authentication succeeds or cleanup runs.
class TlsAdmission final {
  public:
    using Clock = std::chrono::steady_clock;

    TlsAdmission(std::size_t max_pending, std::size_t max_pending_per_ip,
                 std::size_t max_per_second);

    // Pace accept itself, including rejected sockets, to avoid an accept/close
    // busy loop under a flood. The token bucket holds one second of burst.
    [[nodiscard]] Clock::duration accept_delay(Clock::time_point now);
    void record_accept(Clock::time_point now);
    [[nodiscard]] bool try_acquire(std::string_view address, Clock::time_point now);
    void release(std::string_view address);
    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }

    // Five TLS failures in a minute block a source for a minute. Diagnostics
    // share one five-second window across all sources; return suppressed count.
    [[nodiscard]] std::optional<std::size_t> record_failure(std::string_view address,
                                                            Clock::time_point now);

  private:
    void refill(Clock::time_point now);

    std::size_t max_pending_;
    std::size_t max_pending_per_ip_;
    double rate_;
    double tokens_;
    std::optional<Clock::time_point> last_refill_;
    std::unordered_map<std::string, std::size_t> sources_;
    std::size_t pending_{0U};
    protocol::AuthRateLimiter failures_;
    std::optional<Clock::time_point> last_log_;
    std::size_t suppressed_{0U};
};

} // namespace minitun::server
