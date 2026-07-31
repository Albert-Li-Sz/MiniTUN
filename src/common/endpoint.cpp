#include <minitun/common/endpoint.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/error_code.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

#include <minitun/common/error.hpp>

namespace minitun::common {
namespace {

constexpr std::size_t max_domain_length = 253;
constexpr std::size_t max_domain_label_length = 63;
constexpr std::size_t max_endpoint_text_length = 260;
constexpr std::size_t max_port_text_length = 5;

[[nodiscard]] Result<Endpoint> invalid_endpoint(std::string message) {
    return Result<Endpoint>::failure(ErrorCode::invalid_argument, std::move(message));
}

[[nodiscard]] bool contains_whitespace(const std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](const char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    });
}

[[nodiscard]] Result<std::uint16_t> parse_port(const std::string_view value) {
    if (value.empty()) {
        return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                              "endpoint port is empty");
    }
    if (value.size() > max_port_text_length) {
        return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                              "endpoint port has too many digits");
    }

    std::uint32_t port = 0;
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto [position, error] = std::from_chars(begin, end, port);
    if (error != std::errc{} || position != end || port == 0 || port > 65535) {
        return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                              "endpoint port must be a decimal number in 1..65535");
    }

    return static_cast<std::uint16_t>(port);
}

[[nodiscard]] bool is_ascii_letter(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

[[nodiscard]] bool is_ascii_digit(const char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool is_domain_character(const char character) noexcept {
    return is_ascii_letter(character) || is_ascii_digit(character) || character == '-';
}

[[nodiscard]] bool is_ascii_hex_digit(const char character) noexcept {
    return is_ascii_digit(character) || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

[[nodiscard]] bool is_legacy_numeric_component(const std::string_view component) noexcept {
    if (component.empty()) {
        return false;
    }
    if (std::all_of(component.begin(), component.end(), is_ascii_digit)) {
        return true;
    }
    return component.size() > 2 && component[0] == '0' &&
           (component[1] == 'x' || component[1] == 'X') &&
           std::all_of(component.begin() + 2, component.end(), is_ascii_hex_digit);
}

[[nodiscard]] bool is_legacy_numeric_ipv4_name(const std::string_view host) noexcept {
    std::size_t component_start = 0;
    while (component_start < host.size()) {
        const std::size_t component_end = host.find('.', component_start);
        const std::size_t component_size =
            (component_end == std::string_view::npos ? host.size() : component_end) -
            component_start;
        if (!is_legacy_numeric_component(host.substr(component_start, component_size))) {
            return false;
        }
        if (component_end == std::string_view::npos) {
            return true;
        }
        if (component_end + 1 == host.size()) {
            return true;
        }
        component_start = component_end + 1;
    }
    return false;
}

[[nodiscard]] bool is_valid_ipv4_text(const std::string_view host) noexcept {
    if (host.empty() || host.front() == '.' || host.back() == '.') {
        return false;
    }

    std::size_t octet_start = 0;
    std::size_t octet_count = 0;
    while (octet_start < host.size()) {
        const std::size_t octet_end = host.find('.', octet_start);
        const std::size_t octet_size =
            (octet_end == std::string_view::npos ? host.size() : octet_end) - octet_start;
        if (octet_size == 0 || octet_size > 3 || (octet_size > 1 && host[octet_start] == '0')) {
            return false;
        }

        std::uint16_t octet = 0;
        const char* const begin = host.data() + octet_start;
        const char* const end = begin + octet_size;
        const auto [position, error] = std::from_chars(begin, end, octet);
        if (error != std::errc{} || position != end || octet > 255) {
            return false;
        }

        ++octet_count;
        if (octet_end == std::string_view::npos) {
            break;
        }
        octet_start = octet_end + 1;
    }
    return octet_count == 4;
}

[[nodiscard]] bool is_valid_domain_name(std::string_view host) noexcept {
    if (host.empty()) {
        return false;
    }

    if (host.back() == '.') {
        host.remove_suffix(1);
    }
    if (host.empty() || host.back() == '.' || host.size() > max_domain_length) {
        return false;
    }

    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const std::size_t label_end = host.find('.', label_start);
        const std::size_t label_size =
            (label_end == std::string_view::npos ? host.size() : label_end) - label_start;

        if (label_size == 0 || label_size > max_domain_label_length) {
            return false;
        }

        const std::string_view label = host.substr(label_start, label_size);
        if (label.front() == '-' || label.back() == '-' ||
            !std::all_of(label.begin(), label.end(), is_domain_character)) {
            return false;
        }

        if (label_end == std::string_view::npos) {
            break;
        }
        label_start = label_end + 1;
    }

    return true;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    return value;
}

} // namespace

Endpoint::Endpoint(std::string host, const std::uint16_t port, const EndpointKind kind)
    : host_(std::move(host)), port_(port), kind_(kind) {}

Result<Endpoint> Endpoint::parse(const std::string_view value) {
    if (value.empty()) {
        return invalid_endpoint("endpoint is empty");
    }
    if (value.size() > max_endpoint_text_length) {
        return invalid_endpoint("endpoint exceeds the maximum length");
    }
    if (contains_whitespace(value)) {
        return invalid_endpoint("endpoint must not contain whitespace");
    }

    std::string_view host;
    std::string_view port_text;

    if (value.front() == '[') {
        const std::size_t closing_bracket = value.find(']');
        if (closing_bracket == std::string_view::npos || closing_bracket == 1 ||
            closing_bracket + 1 >= value.size() || value[closing_bracket + 1] != ':') {
            return invalid_endpoint("IPv6 endpoints must use the form [address]:port");
        }

        host = value.substr(1, closing_bracket - 1);
        port_text = value.substr(closing_bracket + 2);
        if (host.find('%') != std::string_view::npos) {
            return invalid_endpoint("IPv6 scope identifiers are not supported");
        }

        asio::error_code address_error;
        const asio::ip::address_v6 address =
            asio::ip::make_address_v6(std::string{host}, address_error);
        if (address_error) {
            return invalid_endpoint("endpoint contains an invalid IPv6 address");
        }

        auto port = parse_port(port_text);
        if (!port) {
            return Result<Endpoint>::failure(std::move(port).error());
        }
        return Endpoint{address.to_string(), *port, EndpointKind::ipv6};
    }

    const std::size_t separator = value.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator != value.rfind(':')) {
        return invalid_endpoint("endpoint must use host:port, with brackets around IPv6 addresses");
    }

    host = value.substr(0, separator);
    port_text = value.substr(separator + 1);
    auto port = parse_port(port_text);
    if (!port) {
        return Result<Endpoint>::failure(std::move(port).error());
    }

    if (is_valid_ipv4_text(host)) {
        asio::error_code address_error;
        const asio::ip::address_v4 address =
            asio::ip::make_address_v4(std::string{host}, address_error);
        if (address_error) {
            return invalid_endpoint("endpoint contains an invalid IPv4 address");
        }
        return Endpoint{address.to_string(), *port, EndpointKind::ipv4};
    }
    if (is_legacy_numeric_ipv4_name(host)) {
        return invalid_endpoint("endpoint contains an ambiguous legacy numeric IPv4 address");
    }
    if (!is_valid_domain_name(host)) {
        return invalid_endpoint("endpoint contains an invalid domain name");
    }

    return Endpoint{lower_ascii(std::string{host}), *port, EndpointKind::domain_name};
}

const std::string& Endpoint::host() const noexcept { return host_; }

std::uint16_t Endpoint::port() const noexcept { return port_; }

EndpointKind Endpoint::kind() const noexcept { return kind_; }

bool Endpoint::is_domain() const noexcept { return kind_ == EndpointKind::domain_name; }

bool Endpoint::is_ipv4() const noexcept { return kind_ == EndpointKind::ipv4; }

bool Endpoint::is_ipv6() const noexcept { return kind_ == EndpointKind::ipv6; }

std::string Endpoint::to_string() const {
    if (is_ipv6()) {
        return '[' + host_ + "]:" + std::to_string(port_);
    }
    return host_ + ':' + std::to_string(port_);
}

} // namespace minitun::common
