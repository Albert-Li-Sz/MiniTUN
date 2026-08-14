#include <minitun/remote_protocol.hpp>

#include <memory>
#include <utility>

#include <minitun/common/error.hpp>
#include <minitun/protocol/auth.hpp>

namespace minitun::remote {
namespace {

template <typename T, typename Encoder>
[[nodiscard]] common::Result<std::vector<std::uint8_t>> encode_as(const Message& message,
                                                                  Encoder encoder) {
    const auto* value = std::get_if<T>(&message);
    if (value == nullptr) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::invalid_argument,
            "remote SDK message value does not match its message type");
    }
    return encoder(*value);
}

template <typename T>
[[nodiscard]] common::Result<Message> message_result(common::Result<T> decoded) {
    if (!decoded) {
        return common::Result<Message>::failure(decoded.error());
    }
    return Message{std::in_place_type<T>, std::move(*decoded)};
}

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_payload(const protocol::MessageType type, const Message& message) {
    using protocol::MessageType;
    switch (type) {
    case MessageType::hello:
        return encode_as<protocol::HelloMessage>(message, protocol::encode_hello);
    case MessageType::hello_ack:
        return encode_as<protocol::HelloAckMessage>(message, protocol::encode_hello_ack);
    case MessageType::auth:
        return encode_as<protocol::AuthMessage>(message, protocol::encode_auth);
    case MessageType::auth_ok:
        return encode_as<protocol::AuthOkMessage>(message, protocol::encode_auth_ok);
    case MessageType::auth_error:
        return encode_as<protocol::AuthErrorMessage>(message, protocol::encode_auth_error);
    case MessageType::ping:
    case MessageType::pong:
        return encode_as<protocol::HeartbeatMessage>(message, protocol::encode_heartbeat);
    case MessageType::register_tunnel:
        return encode_as<protocol::RegisterTunnelMessage>(message,
                                                          protocol::encode_register_tunnel);
    case MessageType::register_tunnel_ok:
        return encode_as<protocol::RegisterTunnelOkMessage>(message,
                                                            protocol::encode_register_tunnel_ok);
    case MessageType::register_tunnel_error:
        return encode_as<protocol::RegisterTunnelErrorMessage>(
            message, protocol::encode_register_tunnel_error);
    case MessageType::unregister_tunnel:
        return encode_as<protocol::UnregisterTunnelMessage>(message,
                                                            protocol::encode_unregister_tunnel);
    case MessageType::unregister_tunnel_ok:
        return encode_as<protocol::UnregisterTunnelMessage>(message,
                                                            protocol::encode_unregister_tunnel_ok);
    case MessageType::request_workers:
        return encode_as<protocol::RequestWorkersMessage>(message,
                                                          protocol::encode_request_workers);
    case MessageType::worker_hello:
        return encode_as<protocol::WorkerHelloMessage>(message, protocol::encode_worker_hello);
    case MessageType::worker_accepted:
        return encode_as<protocol::WorkerAcceptedMessage>(message,
                                                          protocol::encode_worker_accepted);
    case MessageType::start_relay:
        return encode_as<protocol::StartRelayMessage>(message, protocol::encode_start_relay);
    case MessageType::local_connect_ok:
        return encode_as<protocol::LocalConnectOkMessage>(message,
                                                          protocol::encode_local_connect_ok);
    case MessageType::local_connect_error:
        return encode_as<protocol::LocalConnectErrorMessage>(message,
                                                             protocol::encode_local_connect_error);
    case MessageType::goaway:
    case MessageType::error: {
        const auto* empty = std::get_if<EmptyMessage>(&message);
        if (empty == nullptr || empty->type != type) {
            return common::Result<std::vector<std::uint8_t>>::failure(
                common::ErrorCode::invalid_argument,
                "remote SDK empty message does not match its message type");
        }
        return std::vector<std::uint8_t>{};
    }
    }
    return common::Result<std::vector<std::uint8_t>>::failure(
        common::ErrorCode::invalid_argument, "remote SDK message type is unsupported");
}

} // namespace

class Decoder::Impl final {
  public:
    explicit Impl(const std::size_t max_frame_size) : decoder(max_frame_size) {}
    protocol::FrameDecoder decoder;
};

Decoder::Decoder(const std::size_t max_frame_size)
    : implementation_(std::make_unique<Impl>(max_frame_size)) {}
Decoder::~Decoder() noexcept = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

common::Result<std::vector<protocol::Frame>>
Decoder::feed(const std::span<const std::uint8_t> bytes) {
    if (implementation_ == nullptr) {
        return common::Result<std::vector<protocol::Frame>>::failure(
            common::ErrorCode::invalid_argument, "remote SDK decoder was moved from");
    }
    return implementation_->decoder.feed(bytes);
}

common::Result<void> Decoder::finish() const {
    if (implementation_ == nullptr) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "remote SDK decoder was moved from");
    }
    return implementation_->decoder.finish();
}
void Decoder::reset() noexcept {
    if (implementation_ != nullptr) {
        implementation_->decoder.reset();
    }
}

common::Result<std::vector<std::uint8_t>> Codec::encode_frame(const protocol::Frame& frame,
                                                              const std::size_t max_frame_size) {
    return protocol::encode_frame(frame, max_frame_size);
}

common::Result<protocol::Frame> Codec::make_frame(const protocol::MessageType type,
                                                  const std::uint64_t request_id,
                                                  const Message& message) {
    auto payload = encode_payload(type, message);
    if (!payload) {
        return common::Result<protocol::Frame>::failure(payload.error());
    }
    return protocol::Frame{type, 0U, request_id, std::move(*payload)};
}

common::Result<Message> Codec::decode_message(const protocol::Frame& frame) {
    using protocol::MessageType;
    switch (frame.type) {
    case MessageType::hello:
        return message_result(protocol::decode_hello(frame.payload));
    case MessageType::hello_ack:
        return message_result(protocol::decode_hello_ack(frame.payload));
    case MessageType::auth:
        return message_result(protocol::decode_auth(frame.payload));
    case MessageType::auth_ok:
        return message_result(protocol::decode_auth_ok(frame.payload));
    case MessageType::auth_error:
        return message_result(protocol::decode_auth_error(frame.payload));
    case MessageType::ping:
    case MessageType::pong:
        return message_result(protocol::decode_heartbeat(frame.payload));
    case MessageType::register_tunnel:
        return message_result(protocol::decode_register_tunnel(frame.payload));
    case MessageType::register_tunnel_ok:
        return message_result(protocol::decode_register_tunnel_ok(frame.payload));
    case MessageType::register_tunnel_error:
        return message_result(protocol::decode_register_tunnel_error(frame.payload));
    case MessageType::unregister_tunnel:
    case MessageType::unregister_tunnel_ok:
        return message_result(protocol::decode_unregister_tunnel(frame.payload));
    case MessageType::request_workers:
        return message_result(protocol::decode_request_workers(frame.payload));
    case MessageType::worker_hello:
        return message_result(protocol::decode_worker_hello(frame.payload));
    case MessageType::worker_accepted:
        return message_result(protocol::decode_worker_accepted(frame.payload));
    case MessageType::start_relay:
        return message_result(protocol::decode_start_relay(frame.payload));
    case MessageType::local_connect_ok:
        return message_result(protocol::decode_local_connect_ok(frame.payload));
    case MessageType::local_connect_error:
        return message_result(protocol::decode_local_connect_error(frame.payload));
    case MessageType::goaway:
    case MessageType::error:
        if (!frame.payload.empty()) {
            return common::Result<Message>::failure(
                common::ErrorCode::protocol_error,
                "remote SDK empty control message contains a payload");
        }
        return Message{EmptyMessage{frame.type}};
    }
    return common::Result<Message>::failure(common::ErrorCode::protocol_error,
                                            "remote SDK message type is unsupported");
}

common::Result<protocol::AuthenticationData> Codec::control_authentication_data(
    const std::string_view psk, const std::string_view client_id, const std::string_view server_id,
    const std::int64_t timestamp_seconds, const protocol::AuthenticationNonce& nonce,
    const protocol::CapabilitySet selected_capabilities) {
    return protocol::compute_authentication_data(psk, client_id, server_id, timestamp_seconds,
                                                 nonce, selected_capabilities);
}

common::Result<protocol::AuthenticationData> Codec::worker_authentication_data(
    const std::string_view psk, const std::string_view client_id, const std::string_view server_id,
    const std::uint64_t session_generation, const std::string_view worker_id,
    const std::int64_t timestamp_seconds, const protocol::AuthenticationNonce& nonce) {
    return protocol::compute_worker_authentication_data(
        psk, client_id, server_id, session_generation, worker_id, timestamp_seconds, nonce);
}

} // namespace minitun::remote
