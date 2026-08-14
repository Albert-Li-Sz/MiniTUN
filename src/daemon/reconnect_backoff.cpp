#include <minitun/daemon/reconnect_backoff.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace minitun::daemon {
namespace {

[[nodiscard]] std::uint64_t random_seed() {
    std::random_device source;
    std::array<std::uint32_t, 2U> words{source(), source()};
    return (static_cast<std::uint64_t>(words[0]) << 32U) | static_cast<std::uint64_t>(words[1]);
}

} // namespace

ReconnectBackoff::ReconnectBackoff() : generator_(random_seed()) {}

ReconnectBackoff::ReconnectBackoff(const std::uint64_t seed) : generator_(seed) {}

std::chrono::milliseconds ReconnectBackoff::next_delay() {
    const auto delay = apply_jitter(base_delay(attempt_), distribution_(generator_));
    if (attempt_ < std::numeric_limits<std::uint32_t>::max()) {
        ++attempt_;
    }
    return delay;
}

void ReconnectBackoff::reset() noexcept { attempt_ = 0U; }

std::uint32_t ReconnectBackoff::attempt() const noexcept { return attempt_; }

std::chrono::milliseconds ReconnectBackoff::base_delay(const std::uint32_t attempt) noexcept {
    constexpr std::array<std::chrono::seconds, 6U> delays{
        std::chrono::seconds{1}, std::chrono::seconds{2},  std::chrono::seconds{4},
        std::chrono::seconds{8}, std::chrono::seconds{16}, std::chrono::seconds{30},
    };
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        delays[std::min<std::size_t>(attempt, delays.size() - 1U)]);
}

std::chrono::milliseconds ReconnectBackoff::apply_jitter(const std::chrono::milliseconds base,
                                                         const double unit_interval) noexcept {
    const double bounded = std::clamp(unit_interval, 0.0, 1.0);
    const double factor = 0.8 + (0.4 * bounded);
    const auto value =
        static_cast<std::int64_t>(std::llround(static_cast<double>(base.count()) * factor));
    return std::chrono::milliseconds{std::max<std::int64_t>(value, 0)};
}

} // namespace minitun::daemon
