#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <minitun/common/result.hpp>

namespace minitun::common {

/// The syntactic kind of host stored by an Endpoint.
enum class EndpointKind : std::uint8_t {
    domain_name,
    ipv4,
    ipv6,
};

/// A validated TCP endpoint.
///
/// Domain names and IPv4 addresses use the "host:port" form. IPv6 literals
/// must be enclosed in brackets, for example "[2001:db8::1]:443".
class Endpoint final {
  public:
    /// Parses an endpoint without performing DNS resolution.
    ///
    /// Parsing is deliberately strict: whitespace, unbracketed IPv6 literals,
    /// malformed numeric IPv4 lookalikes, and ports outside 1..65535 are
    /// rejected.
    [[nodiscard]] static Result<Endpoint> parse(std::string_view value);

    [[nodiscard]] const std::string& host() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] EndpointKind kind() const noexcept;

    [[nodiscard]] bool is_domain() const noexcept;
    [[nodiscard]] bool is_ipv4() const noexcept;
    [[nodiscard]] bool is_ipv6() const noexcept;

    /// Returns a canonical string accepted by parse().
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const Endpoint&, const Endpoint&) = default;

  private:
    Endpoint(std::string host, std::uint16_t port, EndpointKind kind);

    std::string host_;
    std::uint16_t port_;
    EndpointKind kind_;
};

} // namespace minitun::common
