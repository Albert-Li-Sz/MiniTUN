#include <minitun/storage/models.hpp>

#include <array>
#include <string>
#include <utility>

namespace minitun::storage {
namespace {

template <typename Enum> struct EnumEntry final {
    Enum value;
    std::string_view text;
};

constexpr std::array kTunnelProtocols{
    EnumEntry{TunnelProtocol::tcp, std::string_view{"tcp"}},
};

constexpr std::array kServerDesiredStates{
    EnumEntry{ServerDesiredState::enabled, std::string_view{"enabled"}},
    EnumEntry{ServerDesiredState::disabled, std::string_view{"disabled"}},
    EnumEntry{ServerDesiredState::removed, std::string_view{"removed"}},
};

constexpr std::array kServerActualStates{
    EnumEntry{ServerActualState::not_authenticated, std::string_view{"not_authenticated"}},
    EnumEntry{ServerActualState::disconnected, std::string_view{"disconnected"}},
    EnumEntry{ServerActualState::connecting, std::string_view{"connecting"}},
    EnumEntry{ServerActualState::tls_handshake, std::string_view{"tls_handshake"}},
    EnumEntry{ServerActualState::authenticating, std::string_view{"authenticating"}},
    EnumEntry{ServerActualState::online, std::string_view{"online"}},
    EnumEntry{ServerActualState::backoff, std::string_view{"backoff"}},
    EnumEntry{ServerActualState::disabled, std::string_view{"disabled"}},
    EnumEntry{ServerActualState::error, std::string_view{"error"}},
};

constexpr std::array kTunnelDesiredStates{
    EnumEntry{TunnelDesiredState::active, std::string_view{"active"}},
    EnumEntry{TunnelDesiredState::disabled, std::string_view{"disabled"}},
    EnumEntry{TunnelDesiredState::removed, std::string_view{"removed"}},
};

constexpr std::array kTunnelActualStates{
    EnumEntry{TunnelActualState::pending, std::string_view{"pending"}},
    EnumEntry{TunnelActualState::registering, std::string_view{"registering"}},
    EnumEntry{TunnelActualState::active, std::string_view{"active"}},
    EnumEntry{TunnelActualState::failed, std::string_view{"failed"}},
    EnumEntry{TunnelActualState::removing, std::string_view{"removing"}},
    EnumEntry{TunnelActualState::disabled, std::string_view{"disabled"}},
};

template <typename Enum, std::size_t Size>
[[nodiscard]] std::string_view
enum_to_string(const Enum value, const std::array<EnumEntry<Enum>, Size>& entries) noexcept {
    for (const auto& entry : entries) {
        if (entry.value == value) {
            return entry.text;
        }
    }
    return "unknown";
}

template <typename Enum, std::size_t Size>
[[nodiscard]] common::Result<Enum>
enum_from_string(const std::string_view value, const std::array<EnumEntry<Enum>, Size>& entries,
                 const std::string_view type_name) {
    for (const auto& entry : entries) {
        if (entry.text == value) {
            return entry.value;
        }
    }

    std::string message{"unknown "};
    message.append(type_name);
    return common::Result<Enum>::failure(common::ErrorCode::invalid_argument, std::move(message));
}

} // namespace

std::string_view to_string(const TunnelProtocol value) noexcept {
    return enum_to_string(value, kTunnelProtocols);
}

std::string_view to_string(const ServerDesiredState value) noexcept {
    return enum_to_string(value, kServerDesiredStates);
}

std::string_view to_string(const ServerActualState value) noexcept {
    return enum_to_string(value, kServerActualStates);
}

std::string_view to_string(const TunnelDesiredState value) noexcept {
    return enum_to_string(value, kTunnelDesiredStates);
}

std::string_view to_string(const TunnelActualState value) noexcept {
    return enum_to_string(value, kTunnelActualStates);
}

common::Result<TunnelProtocol> tunnel_protocol_from_string(const std::string_view value) {
    return enum_from_string(value, kTunnelProtocols, "tunnel protocol");
}

common::Result<ServerDesiredState> server_desired_state_from_string(const std::string_view value) {
    return enum_from_string(value, kServerDesiredStates, "server desired state");
}

common::Result<ServerActualState> server_actual_state_from_string(const std::string_view value) {
    return enum_from_string(value, kServerActualStates, "server actual state");
}

common::Result<TunnelDesiredState> tunnel_desired_state_from_string(const std::string_view value) {
    return enum_from_string(value, kTunnelDesiredStates, "tunnel desired state");
}

common::Result<TunnelActualState> tunnel_actual_state_from_string(const std::string_view value) {
    return enum_from_string(value, kTunnelActualStates, "tunnel actual state");
}

} // namespace minitun::storage
