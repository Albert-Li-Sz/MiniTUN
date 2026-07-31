#include <minitun/common/port_range.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <minitun/common/error.hpp>

namespace minitun::common {
namespace {

constexpr std::size_t max_port_text_length = 5;
constexpr std::size_t max_port_range_text_length = 11;

[[nodiscard]] Result<PortRange> invalid_range(std::string message) {
    return Result<PortRange>::failure(ErrorCode::invalid_argument, std::move(message));
}

[[nodiscard]] bool contains_whitespace(const std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](const char character) {
        switch (character) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\f':
        case '\v':
            return true;
        default:
            return false;
        }
    });
}

[[nodiscard]] Result<std::uint16_t> parse_port(const std::string_view value) {
    if (value.empty()) {
        return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                              "port range contains an empty port");
    }
    if (value.size() > max_port_text_length) {
        return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                              "port contains too many digits");
    }

    std::uint32_t port = 0;
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto [position, error] = std::from_chars(begin, end, port);
    if (error != std::errc{} || position != end || port == 0 || port > 65535) {
        return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                              "port must be a decimal number in 1..65535");
    }

    return static_cast<std::uint16_t>(port);
}

} // namespace

PortRange::PortRange(const std::uint16_t start, const std::uint16_t end) noexcept
    : start_(start), end_(end) {}

Result<PortRange> PortRange::parse(const std::string_view value) {
    if (value.empty()) {
        return invalid_range("port range is empty");
    }
    if (value.size() > max_port_range_text_length) {
        return invalid_range("port range exceeds the maximum length");
    }
    if (contains_whitespace(value)) {
        return invalid_range("port range must not contain whitespace");
    }

    const std::size_t separator = value.find('-');
    if (separator == std::string_view::npos) {
        auto port = parse_port(value);
        if (!port) {
            return Result<PortRange>::failure(std::move(port).error());
        }
        return PortRange{*port, *port};
    }
    if (separator != value.rfind('-')) {
        return invalid_range("port range must contain at most one '-' separator");
    }

    auto start = parse_port(value.substr(0, separator));
    if (!start) {
        return Result<PortRange>::failure(std::move(start).error());
    }
    auto end = parse_port(value.substr(separator + 1));
    if (!end) {
        return Result<PortRange>::failure(std::move(end).error());
    }
    if (*start > *end) {
        return invalid_range("port range start must not exceed its end");
    }

    return PortRange{*start, *end};
}

std::uint16_t PortRange::start() const noexcept { return start_; }

std::uint16_t PortRange::end() const noexcept { return end_; }

bool PortRange::contains(const std::uint16_t port) const noexcept {
    return port >= start_ && port <= end_;
}

std::uint32_t PortRange::size() const noexcept {
    return static_cast<std::uint32_t>(end_) - static_cast<std::uint32_t>(start_) + 1;
}

std::string PortRange::to_string() const {
    if (start_ == end_) {
        return std::to_string(start_);
    }
    return std::to_string(start_) + '-' + std::to_string(end_);
}

} // namespace minitun::common
