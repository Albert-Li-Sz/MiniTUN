#include <minitun/protocol/state_machine.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] bool client_authenticated_message(const bool sending,
                                                const MessageType type) noexcept {
    if (sending) {
        return type == MessageType::register_tunnel ||
               type == MessageType::unregister_tunnel || type == MessageType::ping ||
               type == MessageType::pong;
    }
    return type == MessageType::register_tunnel_ok ||
           type == MessageType::register_tunnel_error ||
           type == MessageType::unregister_tunnel_ok ||
           type == MessageType::request_workers || type == MessageType::ping ||
           type == MessageType::pong;
}

[[nodiscard]] bool server_authenticated_message(const bool sending,
                                                const MessageType type) noexcept {
    if (sending) {
        return type == MessageType::register_tunnel_ok ||
               type == MessageType::register_tunnel_error ||
               type == MessageType::unregister_tunnel_ok ||
               type == MessageType::request_workers || type == MessageType::ping ||
               type == MessageType::pong;
    }
    return type == MessageType::register_tunnel ||
           type == MessageType::unregister_tunnel || type == MessageType::ping ||
           type == MessageType::pong;
}

} // namespace

std::string_view to_string(const ConnectionState state) noexcept {
    switch (state) {
    case ConnectionState::client_send_hello:
        return "client_send_hello";
    case ConnectionState::client_wait_hello_ack:
        return "client_wait_hello_ack";
    case ConnectionState::client_send_auth:
        return "client_send_auth";
    case ConnectionState::client_wait_auth_result:
        return "client_wait_auth_result";
    case ConnectionState::server_wait_hello:
        return "server_wait_hello";
    case ConnectionState::server_send_hello_ack:
        return "server_send_hello_ack";
    case ConnectionState::server_wait_auth:
        return "server_wait_auth";
    case ConnectionState::server_send_auth_result:
        return "server_send_auth_result";
    case ConnectionState::authenticated:
        return "authenticated";
    case ConnectionState::client_send_worker_hello:
        return "client_send_worker_hello";
    case ConnectionState::client_wait_worker_accepted:
        return "client_wait_worker_accepted";
    case ConnectionState::server_wait_worker_hello:
        return "server_wait_worker_hello";
    case ConnectionState::server_send_worker_accepted:
        return "server_send_worker_accepted";
    case ConnectionState::worker_idle:
        return "worker_idle";
    case ConnectionState::client_connecting_local:
        return "client_connecting_local";
    case ConnectionState::server_wait_local_result:
        return "server_wait_local_result";
    case ConnectionState::relay:
        return "relay";
    case ConnectionState::closing:
        return "closing";
    }
    return "unknown";
}

StateMachine::StateMachine(const PeerRole role, const ConnectionKind kind) noexcept
    : role_(role), kind_(kind),
      state_(kind == ConnectionKind::control
                 ? (role == PeerRole::client ? ConnectionState::client_send_hello
                                             : ConnectionState::server_wait_hello)
                 : (role == PeerRole::client ? ConnectionState::client_send_worker_hello
                                             : ConnectionState::server_wait_worker_hello)) {}

common::Result<void> StateMachine::on_send(const MessageType type) {
    return transition(Direction::send, type);
}

common::Result<void> StateMachine::on_receive(const MessageType type) {
    return transition(Direction::receive, type);
}

PeerRole StateMachine::role() const noexcept { return role_; }

ConnectionKind StateMachine::kind() const noexcept { return kind_; }

ConnectionState StateMachine::state() const noexcept { return state_; }

bool StateMachine::framed_mode() const noexcept {
    return state_ != ConnectionState::relay && state_ != ConnectionState::closing;
}

common::Result<void> StateMachine::transition(const Direction direction, const MessageType type) {
    if (state_ == ConnectionState::relay || state_ == ConnectionState::closing) {
        return invalid_transition(type);
    }

    if (type == MessageType::goaway || type == MessageType::error) {
        state_ = ConnectionState::closing;
        return common::Result<void>::success();
    }

    if (kind_ == ConnectionKind::control && !is_control_message(type)) {
        return invalid_transition(type);
    }
    if (kind_ == ConnectionKind::worker && !is_worker_message(type)) {
        return invalid_transition(type);
    }

    switch (state_) {
    case ConnectionState::client_send_hello:
        if (direction == Direction::send && type == MessageType::hello) {
            state_ = ConnectionState::client_wait_hello_ack;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::client_wait_hello_ack:
        if (direction == Direction::receive && type == MessageType::hello_ack) {
            state_ = ConnectionState::client_send_auth;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::client_send_auth:
        if (direction == Direction::send && type == MessageType::auth) {
            state_ = ConnectionState::client_wait_auth_result;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::client_wait_auth_result:
        if (direction == Direction::receive && type == MessageType::auth_ok) {
            state_ = ConnectionState::authenticated;
            return common::Result<void>::success();
        }
        if (direction == Direction::receive && type == MessageType::auth_error) {
            state_ = ConnectionState::closing;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::server_wait_hello:
        if (direction == Direction::receive && type == MessageType::hello) {
            state_ = ConnectionState::server_send_hello_ack;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::server_send_hello_ack:
        if (direction == Direction::send && type == MessageType::hello_ack) {
            state_ = ConnectionState::server_wait_auth;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::server_wait_auth:
        if (direction == Direction::receive && type == MessageType::auth) {
            state_ = ConnectionState::server_send_auth_result;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::server_send_auth_result:
        if (direction == Direction::send && type == MessageType::auth_ok) {
            state_ = ConnectionState::authenticated;
            return common::Result<void>::success();
        }
        if (direction == Direction::send && type == MessageType::auth_error) {
            state_ = ConnectionState::closing;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::authenticated:
        if ((role_ == PeerRole::client &&
             client_authenticated_message(direction == Direction::send, type)) ||
            (role_ == PeerRole::server &&
             server_authenticated_message(direction == Direction::send, type))) {
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::client_send_worker_hello:
        if (direction == Direction::send && type == MessageType::worker_hello) {
            state_ = ConnectionState::client_wait_worker_accepted;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::client_wait_worker_accepted:
        if (direction == Direction::receive && type == MessageType::worker_accepted) {
            state_ = ConnectionState::worker_idle;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::server_wait_worker_hello:
        if (direction == Direction::receive && type == MessageType::worker_hello) {
            state_ = ConnectionState::server_send_worker_accepted;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::server_send_worker_accepted:
        if (direction == Direction::send && type == MessageType::worker_accepted) {
            state_ = ConnectionState::worker_idle;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::worker_idle:
        if (role_ == PeerRole::client && direction == Direction::receive &&
            type == MessageType::start_relay) {
            state_ = ConnectionState::client_connecting_local;
            return common::Result<void>::success();
        }
        if (role_ == PeerRole::server && direction == Direction::send &&
            type == MessageType::start_relay) {
            state_ = ConnectionState::server_wait_local_result;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::client_connecting_local:
        if (direction == Direction::send && type == MessageType::local_connect_ok) {
            state_ = ConnectionState::relay;
            return common::Result<void>::success();
        }
        if (direction == Direction::send && type == MessageType::local_connect_error) {
            state_ = ConnectionState::closing;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::server_wait_local_result:
        if (direction == Direction::receive && type == MessageType::local_connect_ok) {
            state_ = ConnectionState::relay;
            return common::Result<void>::success();
        }
        if (direction == Direction::receive && type == MessageType::local_connect_error) {
            state_ = ConnectionState::closing;
            return common::Result<void>::success();
        }
        break;
    case ConnectionState::relay:
    case ConnectionState::closing:
        break;
    }

    return invalid_transition(type);
}

common::Result<void> StateMachine::invalid_transition(const MessageType) const {
    return common::Result<void>::failure(
        common::ErrorCode::protocol_error,
        "remote protocol message is invalid for the current connection state");
}

} // namespace minitun::protocol
