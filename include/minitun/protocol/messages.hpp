#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>

namespace minitun::protocol {

inline constexpr std::size_t kAuthenticationNonceSize = 32U;
inline constexpr std::size_t kAuthenticationDataSize = 32U;
inline constexpr std::size_t kMaxProtocolIdentifierBytes = 64U;
inline constexpr std::size_t kMaxTunnelBindHostBytes = 253U;
inline constexpr std::uint32_t kDefaultHeartbeatIntervalMilliseconds = 5'000U;
inline constexpr std::uint16_t kDefaultMinIdleWorkers = 2U;
inline constexpr std::uint16_t kDefaultMaxIdleWorkers = 32U;
inline constexpr std::uint16_t kMinimumWorkerIdleTimeoutSeconds = 1U;
inline constexpr std::uint16_t kMaximumWorkerIdleTimeoutSeconds = 300U;
inline constexpr std::uint16_t kWorkerIdleTimeoutGraceSeconds = 5U;
inline constexpr std::uint64_t kMaximumNegotiatedHeartbeatSequence = (1ULL << 39U) - 1U;

using CapabilitySet = std::uint64_t;

enum class Capability : CapabilitySet {
    pipelined_control = 1ULL << 0U,
    per_client_policy = 1ULL << 1U,
    tunnel_revisions = 1ULL << 2U,
    client_certificate_binding = 1ULL << 3U,
    multiplexed_streams = 1ULL << 4U,
    udp_datagrams = 1ULL << 5U,
    socks5_proxy = 1ULL << 6U,
    p2p_rendezvous = 1ULL << 7U,
    tcp_simultaneous_open = 1ULL << 8U,
    proxy_protocol = 1ULL << 9U,
    /// The server reports the daemon Worker's own public address (as observed
    /// on its authenticated TLS connection) so the P2P offer can advertise a
    /// NAT-reachable candidate instead of the Worker's private address.
    worker_observed_endpoint = 1ULL << 10U,
};

[[nodiscard]] constexpr CapabilitySet capability_bit(const Capability capability) noexcept {
    return static_cast<CapabilitySet>(capability);
}

inline constexpr CapabilitySet kRequiredCapabilities =
    capability_bit(Capability::pipelined_control) | capability_bit(Capability::per_client_policy) |
    capability_bit(Capability::tunnel_revisions);
inline constexpr CapabilitySet kSupportedCapabilities =
    kRequiredCapabilities | capability_bit(Capability::client_certificate_binding) |
    capability_bit(Capability::udp_datagrams) | capability_bit(Capability::socks5_proxy) |
    capability_bit(Capability::p2p_rendezvous) |
    capability_bit(Capability::tcp_simultaneous_open) |
    capability_bit(Capability::proxy_protocol) |
    capability_bit(Capability::worker_observed_endpoint);

enum class TunnelMode : std::uint8_t {
    tcp = 0U,
    udp = 1U,
    socks5 = 2U,
    p2p = 3U,
};

[[nodiscard]] constexpr Capability required_capability(const TunnelMode mode) noexcept {
    switch (mode) {
    case TunnelMode::udp:
        return Capability::udp_datagrams;
    case TunnelMode::socks5:
        return Capability::socks5_proxy;
    case TunnelMode::p2p:
        return Capability::p2p_rendezvous;
    case TunnelMode::tcp:
        return Capability::pipelined_control;
    }
    return Capability::pipelined_control;
}

[[nodiscard]] constexpr bool supports_tunnel_mode(const CapabilitySet capabilities,
                                                  const TunnelMode mode) noexcept {
    const auto required = capability_bit(required_capability(mode));
    return (capabilities & required) == required;
}

using AuthenticationNonce = std::array<std::uint8_t, kAuthenticationNonceSize>;
using AuthenticationData = std::array<std::uint8_t, kAuthenticationDataSize>;

struct HelloMessage final {
    std::string client_id;
    CapabilitySet capabilities{kSupportedCapabilities};

    friend bool operator==(const HelloMessage&, const HelloMessage&) = default;
};

struct HelloAckMessage final {
    std::string server_id;
    std::int64_t server_time_seconds{0};
    AuthenticationNonce nonce{};
    CapabilitySet selected_capabilities{kRequiredCapabilities};

    friend bool operator==(const HelloAckMessage&, const HelloAckMessage&) = default;
};

struct AuthMessage final {
    std::string client_id;
    std::int64_t timestamp_seconds{0};
    AuthenticationNonce nonce{};
    AuthenticationData authentication_data{};
    CapabilitySet selected_capabilities{kRequiredCapabilities};

    friend bool operator==(const AuthMessage&, const AuthMessage&) = default;
};

struct AuthOkMessage final {
    std::uint64_t session_generation{0U};
    std::uint32_t heartbeat_interval_milliseconds{kDefaultHeartbeatIntervalMilliseconds};
    std::uint16_t min_idle_workers{kDefaultMinIdleWorkers};
    std::uint16_t max_idle_workers{kDefaultMaxIdleWorkers};

    friend bool operator==(const AuthOkMessage&, const AuthOkMessage&) = default;
};

struct AuthErrorMessage final {
    common::ErrorCode code{common::ErrorCode::authentication_failed};

    friend bool operator==(const AuthErrorMessage&, const AuthErrorMessage&) = default;
};

struct HeartbeatMessage final {
    std::uint64_t sequence{0U};

    friend bool operator==(const HeartbeatMessage&, const HeartbeatMessage&) = default;
};

struct RegisterTunnelMessage final {
    std::string tunnel_id;
    std::string bind_host;
    std::uint16_t bind_port{0U};
    std::uint64_t desired_revision{1U};
    TunnelMode mode{TunnelMode::tcp};

    friend bool operator==(const RegisterTunnelMessage&, const RegisterTunnelMessage&) = default;
};

struct RegisterTunnelOkMessage final {
    std::string tunnel_id;
    std::uint64_t desired_revision{1U};

    friend bool operator==(const RegisterTunnelOkMessage&,
                           const RegisterTunnelOkMessage&) = default;
};

struct RegisterTunnelErrorMessage final {
    std::string tunnel_id;
    common::ErrorCode code{common::ErrorCode::internal_error};
    std::uint64_t desired_revision{1U};

    friend bool operator==(const RegisterTunnelErrorMessage&,
                           const RegisterTunnelErrorMessage&) = default;
};

struct UnregisterTunnelMessage final {
    std::string tunnel_id;
    std::uint64_t desired_revision{1U};

    friend bool operator==(const UnregisterTunnelMessage&,
                           const UnregisterTunnelMessage&) = default;
};

using UnregisterTunnelOkMessage = UnregisterTunnelMessage;

struct RequestWorkersMessage final {
    std::uint16_t count{0U};

    friend bool operator==(const RequestWorkersMessage&, const RequestWorkersMessage&) = default;
};

struct WorkerHelloMessage final {
    std::string client_id;
    std::uint64_t session_generation{0U};
    std::string worker_id;
    std::int64_t timestamp_seconds{0};
    AuthenticationNonce nonce{};
    AuthenticationData authentication_data{};

    friend bool operator==(const WorkerHelloMessage&, const WorkerHelloMessage&) = default;
};

struct WorkerAcceptedMessage final {
    std::string worker_id;

    friend bool operator==(const WorkerAcceptedMessage&, const WorkerAcceptedMessage&) = default;
};

struct StartRelayMessage final {
    std::string tunnel_id;
    std::string connection_id;
    TunnelMode mode{TunnelMode::tcp};
    /// Public client endpoint observed by the server; used for PROXY
    /// protocol headers and TCP simultaneous open. Both or neither set.
    std::optional<std::string> source_host{std::nullopt};
    std::optional<std::uint16_t> source_port{std::nullopt};
    /// The daemon Worker's own public address as observed by the server. Used
    /// to advertise a NAT-reachable P2P candidate; present only when the
    /// source endpoint extension is present and the server negotiated the
    /// worker_observed_endpoint capability.
    std::optional<std::string> worker_observed_host{std::nullopt};

    friend bool operator==(const StartRelayMessage&, const StartRelayMessage&) = default;
};

struct LocalConnectOkMessage final {
    std::string connection_id;

    friend bool operator==(const LocalConnectOkMessage&, const LocalConnectOkMessage&) = default;
};

struct LocalConnectErrorMessage final {
    std::string connection_id;
    common::ErrorCode code{common::ErrorCode::local_connect_failed};

    friend bool operator==(const LocalConnectErrorMessage&,
                           const LocalConnectErrorMessage&) = default;
};

[[nodiscard]] common::Result<std::vector<std::uint8_t>> encode_hello(const HelloMessage& message);
[[nodiscard]] common::Result<HelloMessage> decode_hello(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_hello_ack(const HelloAckMessage& message);
[[nodiscard]] common::Result<HelloAckMessage>
decode_hello_ack(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>> encode_auth(const AuthMessage& message);
[[nodiscard]] common::Result<AuthMessage> decode_auth(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_auth_ok(const AuthOkMessage& message);
[[nodiscard]] common::Result<AuthOkMessage>
decode_auth_ok(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_auth_error(const AuthErrorMessage& message);
[[nodiscard]] common::Result<AuthErrorMessage>
decode_auth_error(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_heartbeat(const HeartbeatMessage& message);
[[nodiscard]] common::Result<HeartbeatMessage>
decode_heartbeat(const std::vector<std::uint8_t>& payload);

/// Adds backward-compatible Worker timeout metadata to a heartbeat sequence.
/// Legacy peers treat the returned value as opaque and echo it unchanged,
/// while newer peers can recover the configured timeout.
[[nodiscard]] common::Result<std::uint64_t>
encode_worker_timeout_heartbeat_sequence(std::uint64_t sequence,
                                         std::uint16_t worker_idle_timeout_seconds);
[[nodiscard]] std::optional<std::uint16_t>
decode_worker_idle_timeout_seconds(std::uint64_t sequence) noexcept;

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_register_tunnel(const RegisterTunnelMessage& message);
[[nodiscard]] common::Result<RegisterTunnelMessage>
decode_register_tunnel(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_register_tunnel_ok(const RegisterTunnelOkMessage& message);
[[nodiscard]] common::Result<RegisterTunnelOkMessage>
decode_register_tunnel_ok(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_register_tunnel_error(const RegisterTunnelErrorMessage& message);
[[nodiscard]] common::Result<RegisterTunnelErrorMessage>
decode_register_tunnel_error(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_unregister_tunnel(const UnregisterTunnelMessage& message);
[[nodiscard]] common::Result<UnregisterTunnelMessage>
decode_unregister_tunnel(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_unregister_tunnel_ok(const UnregisterTunnelOkMessage& message);
[[nodiscard]] common::Result<UnregisterTunnelOkMessage>
decode_unregister_tunnel_ok(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_request_workers(const RequestWorkersMessage& message);
[[nodiscard]] common::Result<RequestWorkersMessage>
decode_request_workers(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_worker_hello(const WorkerHelloMessage& message);
[[nodiscard]] common::Result<WorkerHelloMessage>
decode_worker_hello(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_worker_accepted(const WorkerAcceptedMessage& message);
[[nodiscard]] common::Result<WorkerAcceptedMessage>
decode_worker_accepted(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_start_relay(const StartRelayMessage& message);
[[nodiscard]] common::Result<StartRelayMessage>
decode_start_relay(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_local_connect_ok(const LocalConnectOkMessage& message);
[[nodiscard]] common::Result<LocalConnectOkMessage>
decode_local_connect_ok(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_local_connect_error(const LocalConnectErrorMessage& message);
[[nodiscard]] common::Result<LocalConnectErrorMessage>
decode_local_connect_error(const std::vector<std::uint8_t>& payload);

} // namespace minitun::protocol
