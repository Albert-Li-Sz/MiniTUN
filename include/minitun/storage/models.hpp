#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>

namespace minitun::storage {

inline constexpr std::size_t kMaxNameBytes = 64;
inline constexpr std::size_t kMaxCredentialReferenceBytes = 256;
inline constexpr std::size_t kMaxRemoteServerIdBytes = 256;
inline constexpr std::size_t kMaxTlsServerNameBytes = 253;
inline constexpr std::size_t kMaxErrorMessageBytes = 4096;

inline constexpr std::size_t kDefaultMaxServers = 128;
inline constexpr std::size_t kDefaultMaxTunnels = 4096;

struct StorageLimits final {
    std::size_t max_servers{kDefaultMaxServers};
    std::size_t max_tunnels{kDefaultMaxTunnels};

    friend bool operator==(const StorageLimits&, const StorageLimits&) = default;
};

enum class TunnelProtocol : std::uint8_t {
    tcp,
    udp,
    socks5,
    p2p,
};

enum class ServerDesiredState : std::uint8_t {
    enabled,
    disabled,
    removed,
};

enum class ServerActualState : std::uint8_t {
    not_authenticated,
    disconnected,
    connecting,
    tls_handshake,
    authenticating,
    online,
    backoff,
    disabled,
    error,
};

enum class TunnelDesiredState : std::uint8_t {
    active,
    disabled,
    removed,
};

enum class TunnelActualState : std::uint8_t {
    pending,
    registering,
    active,
    failed,
    removing,
    disabled,
};

[[nodiscard]] std::string_view to_string(TunnelProtocol value) noexcept;
[[nodiscard]] std::string_view to_string(ServerDesiredState value) noexcept;
[[nodiscard]] std::string_view to_string(ServerActualState value) noexcept;
[[nodiscard]] std::string_view to_string(TunnelDesiredState value) noexcept;
[[nodiscard]] std::string_view to_string(TunnelActualState value) noexcept;

[[nodiscard]] common::Result<TunnelProtocol> tunnel_protocol_from_string(std::string_view value);
[[nodiscard]] common::Result<ServerDesiredState>
server_desired_state_from_string(std::string_view value);
[[nodiscard]] common::Result<ServerActualState>
server_actual_state_from_string(std::string_view value);
[[nodiscard]] common::Result<TunnelDesiredState>
tunnel_desired_state_from_string(std::string_view value);
[[nodiscard]] common::Result<TunnelActualState>
tunnel_actual_state_from_string(std::string_view value);

/// A fully validated server row.
///
/// String lengths are measured in UTF-8 bytes at repository boundaries.
/// Timestamps are Unix milliseconds.
struct ServerRecord final {
    common::Id id;
    std::optional<std::string> name{std::nullopt};
    common::Endpoint endpoint;
    std::optional<std::string> credential_ref{std::nullopt};
    std::optional<std::string> remote_server_id{std::nullopt};

    ServerDesiredState desired_state;
    ServerActualState actual_state;

    std::optional<common::ErrorCode> last_error_code{std::nullopt};
    std::optional<std::string> last_error_message{std::nullopt};

    std::uint32_t reconnect_attempt;
    std::optional<std::int64_t> latency_ms{std::nullopt};

    std::int64_t created_at_unix_ms;
    std::int64_t updated_at_unix_ms;

    // Configuration fields introduced by schema v4.  They intentionally live
    // after the v1-v3 fields so source-level aggregate initialization from
    // older callers receives safe defaults during migration.
    std::optional<std::string> tls_server_name{std::nullopt};
    std::optional<std::string> ca_credential_ref{std::nullopt};
    std::optional<std::string> client_certificate_ref{std::nullopt};
    std::optional<std::string> client_private_key_ref{std::nullopt};
    std::uint64_t config_revision{1U};
    bool managed_by_config{false};

    friend bool operator==(const ServerRecord&, const ServerRecord&) = default;
};

/// A fully validated tunnel row.
///
/// Both endpoints are canonical common::Endpoint values. Timestamps are Unix
/// milliseconds.
struct TunnelRecord final {
    common::Id id;
    std::optional<std::string> name{std::nullopt};
    common::Id server_id;

    TunnelProtocol protocol;
    common::Endpoint local_endpoint;
    common::Endpoint remote_endpoint;

    TunnelDesiredState desired_state;
    TunnelActualState actual_state;

    std::optional<common::ErrorCode> last_error_code{std::nullopt};
    std::optional<std::string> last_error_message{std::nullopt};

    std::int64_t created_at_unix_ms;
    std::int64_t updated_at_unix_ms;
    std::optional<std::int64_t> last_synced_at_unix_ms{std::nullopt};

    std::uint64_t config_revision{1U};
    bool managed_by_config{false};
    bool proxy_protocol{false};

    friend bool operator==(const TunnelRecord&, const TunnelRecord&) = default;
};

/// The complete, normalized state loaded before daemon runtime objects start.
struct RecoverySnapshot final {
    std::vector<ServerRecord> servers;
    std::vector<TunnelRecord> tunnels;

    friend bool operator==(const RecoverySnapshot&, const RecoverySnapshot&) = default;
};

} // namespace minitun::storage
