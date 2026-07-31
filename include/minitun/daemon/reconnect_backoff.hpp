#pragma once

#include <chrono>
#include <cstdint>
#include <random>

namespace minitun::daemon {

class ReconnectBackoff final {
  public:
    ReconnectBackoff();
    explicit ReconnectBackoff(std::uint64_t seed);

    [[nodiscard]] std::chrono::milliseconds next_delay();
    void reset() noexcept;

    [[nodiscard]] std::uint32_t attempt() const noexcept;

    [[nodiscard]] static std::chrono::milliseconds base_delay(std::uint32_t attempt) noexcept;
    [[nodiscard]] static std::chrono::milliseconds apply_jitter(std::chrono::milliseconds base,
                                                                double unit_interval) noexcept;

  private:
    std::mt19937_64 generator_;
    std::uniform_real_distribution<double> distribution_{0.0, 1.0};
    std::uint32_t attempt_{0U};
};

} // namespace minitun::daemon
