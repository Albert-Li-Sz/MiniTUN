#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>

namespace minitun::protocol {

inline constexpr std::size_t kAuthenticationNonceSize = 32U;
inline constexpr std::size_t kAuthenticationDataSize = 32U;
inline constexpr std::size_t kMaxProtocolIdentifierBytes = 64U;
inline constexpr std::uint32_t kDefaultHeartbeatIntervalMilliseconds = 5'000U;
inline constexpr std::uint16_t kDefaultMinIdleWorkers = 2U;
inline constexpr std::uint16_t kDefaultMaxIdleWorkers = 32U;

using AuthenticationNonce = std::array<std::uint8_t, kAuthenticationNonceSize>;
using AuthenticationData = std::array<std::uint8_t, kAuthenticationDataSize>;

struct HelloMessage final {
    std::string client_id;

    friend bool operator==(const HelloMessage&, const HelloMessage&) = default;
};

struct HelloAckMessage final {
    std::string server_id;
    std::int64_t server_time_seconds{0};
    AuthenticationNonce nonce{};

    friend bool operator==(const HelloAckMessage&, const HelloAckMessage&) = default;
};

struct AuthMessage final {
    std::string client_id;
    std::int64_t timestamp_seconds{0};
    AuthenticationNonce nonce{};
    AuthenticationData authentication_data{};

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

[[nodiscard]] common::Result<std::vector<std::uint8_t>> encode_hello(const HelloMessage& message);
[[nodiscard]] common::Result<HelloMessage> decode_hello(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_hello_ack(const HelloAckMessage& message);
[[nodiscard]] common::Result<HelloAckMessage>
decode_hello_ack(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>> encode_auth(const AuthMessage& message);
[[nodiscard]] common::Result<AuthMessage> decode_auth(const std::vector<std::uint8_t>& payload);

[[nodiscard]] common::Result<std::vector<std::uint8_t>> encode_auth_ok(const AuthOkMessage& message);
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

} // namespace minitun::protocol
