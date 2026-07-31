#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <minitun/common/result.hpp>

namespace minitun::common {

/// An inclusive, non-empty TCP port range.
class PortRange final {
  public:
    /// Parses either a single port ("443") or an inclusive range
    /// ("6000-6999"). Whitespace and ports outside 1..65535 are rejected.
    [[nodiscard]] static Result<PortRange> parse(std::string_view value);

    [[nodiscard]] std::uint16_t start() const noexcept;
    [[nodiscard]] std::uint16_t end() const noexcept;
    [[nodiscard]] bool contains(std::uint16_t port) const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

    /// Returns a canonical single-port or "start-end" representation.
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const PortRange&, const PortRange&) = default;

  private:
    PortRange(std::uint16_t start, std::uint16_t end) noexcept;

    std::uint16_t start_;
    std::uint16_t end_;
};

} // namespace minitun::common
