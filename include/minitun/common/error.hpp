#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace minitun::common {

/// Stable, transport-independent error codes used by every MiniTun component.
///
/// Keep the spelling returned by to_string() stable: it is part of the CLI,
/// IPC, protocol diagnostics, and structured-log contracts.
enum class ErrorCode : std::uint8_t {
    ok = 0,
    invalid_argument,
    not_found,
    already_exists,
    permission_denied,
    not_authenticated,
    authentication_failed,
    connection_failed,
    connection_timeout,
    remote_port_in_use,
    local_connect_failed,
    protocol_error,
    frame_too_large,
    unsupported_version,
    resource_exhausted,
    database_error,
    tls_error,
    ipc_error,
    internal_error,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

[[nodiscard]] std::optional<ErrorCode> error_code_from_string(std::string_view value) noexcept;

/// A non-sensitive description of an operation failure.
///
/// Error messages may be shown to users and written to logs. Callers must not
/// include tokens, private keys, raw authentication data, or credentials.
// clang-analyzer models the Error stored in Result<T> as default-constructed
// garbage even though Result has no default constructor; the diagnostic is a
// false positive on the implicit copy/move constructor.
class Error final { // NOLINT(clang-analyzer-core.uninitialized.Assign)
  public:
    explicit Error(ErrorCode code, std::string message = {});

    [[nodiscard]] ErrorCode code() const noexcept;
    [[nodiscard]] const std::string& message() const noexcept;

    /// Returns a copy with a short, non-sensitive context prefix.
    [[nodiscard]] Error with_context(std::string_view context) const;

    friend bool operator==(const Error&, const Error&) = default;

  private:
    ErrorCode code_{ErrorCode::ok};
    std::string message_{};
};

std::ostream& operator<<(std::ostream& stream, const Error& error);

} // namespace minitun::common
