#pragma once

#include <cstdint>
#include <string_view>

#include <minitun/common/result.hpp>
#include <minitun/protocol/frame.hpp>

namespace minitun::protocol {

enum class PeerRole : std::uint8_t {
    client,
    server,
};

enum class ConnectionKind : std::uint8_t {
    control,
    worker,
};

enum class ConnectionState : std::uint8_t {
    client_send_hello,
    client_wait_hello_ack,
    client_send_auth,
    client_wait_auth_result,
    server_wait_hello,
    server_send_hello_ack,
    server_wait_auth,
    server_send_auth_result,
    authenticated,
    client_send_worker_hello,
    client_wait_worker_accepted,
    server_wait_worker_hello,
    server_send_worker_accepted,
    worker_idle,
    client_connecting_local,
    server_wait_local_result,
    relay,
    closing,
};

[[nodiscard]] std::string_view to_string(ConnectionState state) noexcept;

class StateMachine final {
  public:
    StateMachine(PeerRole role, ConnectionKind kind) noexcept;

    [[nodiscard]] common::Result<void> on_send(MessageType type);
    [[nodiscard]] common::Result<void> on_receive(MessageType type);

    [[nodiscard]] PeerRole role() const noexcept;
    [[nodiscard]] ConnectionKind kind() const noexcept;
    [[nodiscard]] ConnectionState state() const noexcept;
    [[nodiscard]] bool framed_mode() const noexcept;

  private:
    enum class Direction : std::uint8_t {
        send,
        receive,
    };

    [[nodiscard]] common::Result<void> transition(Direction direction, MessageType type);
    [[nodiscard]] common::Result<void> invalid_transition(MessageType type) const;

    PeerRole role_;
    ConnectionKind kind_;
    ConnectionState state_;
};

} // namespace minitun::protocol
