#include <minitun/protocol/messages.hpp>

#include <bit>
#include <span>
#include <utility>

#include <minitun/common/id.hpp>
#include <minitun/protocol/codec.hpp>

namespace minitun::protocol {
namespace {

// Tagged heartbeat layout: 16-bit "MT" marker, 9-bit timeout, 39-bit sequence.
inline constexpr std::uint64_t kHeartbeatMetadataTag = 0x4d54ULL << 48U;
inline constexpr std::uint64_t kHeartbeatMetadataTagMask = 0xffffULL << 48U;
inline constexpr std::uint64_t kHeartbeatTimeoutMask = 0x1ffULL;
inline constexpr unsigned int kHeartbeatTimeoutShift = 39U;

[[nodiscard]] common::Result<void> validate_client_id(const std::string_view value) {
    const auto parsed = common::Id::parse(value, common::IdKind::client);
    if (!parsed) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote message contains an invalid client ID");
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> validate_server_id(const std::string_view value) {
    const auto parsed = common::Id::parse(value, common::IdKind::server);
    if (!parsed) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote message contains an invalid server ID");
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> validate_tunnel_id(const std::string_view value) {
    const auto parsed = common::Id::parse(value, common::IdKind::tunnel);
    if (!parsed) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote message contains an invalid tunnel ID");
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> validate_connection_id(const std::string_view value) {
    const auto parsed = common::Id::parse(value, common::IdKind::connection);
    if (!parsed) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote message contains an invalid connection ID");
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_tunnel_id(const std::string_view tunnel_id) {
    auto valid = validate_tunnel_id(tunnel_id);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    PayloadWriter writer;
    if (auto written = writer.write_string(tunnel_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

[[nodiscard]] common::Result<std::string>
decode_tunnel_id(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto tunnel_id = reader.read_string(kMaxProtocolIdentifierBytes);
    if (!tunnel_id) {
        return common::Result<std::string>::failure(tunnel_id.error());
    }
    auto valid = validate_tunnel_id(*tunnel_id);
    if (!valid) {
        return common::Result<std::string>::failure(valid.error());
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<std::string>::failure(ended.error());
    }
    return tunnel_id;
}

template <std::size_t Size>
[[nodiscard]] common::Result<std::array<std::uint8_t, Size>>
read_fixed_bytes(PayloadReader& reader) {
    auto bytes = reader.read_bytes(Size);
    if (!bytes) {
        return common::Result<std::array<std::uint8_t, Size>>::failure(bytes.error());
    }
    if (bytes->size() != Size) {
        return common::Result<std::array<std::uint8_t, Size>>::failure(
            common::ErrorCode::protocol_error,
            "remote message contains a fixed-size field with the wrong length");
    }

    std::array<std::uint8_t, Size> value{};
    std::copy(bytes->begin(), bytes->end(), value.begin());
    return value;
}

[[nodiscard]] common::Result<void> validate_auth_ok(const AuthOkMessage& message) {
    if (message.session_generation == 0U) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "session generation must not be zero");
    }
    if (message.heartbeat_interval_milliseconds < 1'000U ||
        message.heartbeat_interval_milliseconds > 60'000U) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "heartbeat interval is outside the protocol limits");
    }
    if (message.min_idle_workers > message.max_idle_workers || message.max_idle_workers > 128U) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "worker limits are inconsistent");
    }
    return common::Result<void>::success();
}

} // namespace

common::Result<std::uint64_t>
encode_worker_timeout_heartbeat_sequence(const std::uint64_t sequence,
                                         const std::uint16_t worker_idle_timeout_seconds) {
    if (sequence == 0U || sequence > kMaximumNegotiatedHeartbeatSequence ||
        worker_idle_timeout_seconds < kMinimumWorkerIdleTimeoutSeconds ||
        worker_idle_timeout_seconds > kMaximumWorkerIdleTimeoutSeconds) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "heartbeat sequence or Worker idle timeout is outside its limit"};
    }
    return kHeartbeatMetadataTag |
           (static_cast<std::uint64_t>(worker_idle_timeout_seconds) << kHeartbeatTimeoutShift) |
           sequence;
}

std::optional<std::uint16_t>
decode_worker_idle_timeout_seconds(const std::uint64_t sequence) noexcept {
    if ((sequence & kHeartbeatMetadataTagMask) != kHeartbeatMetadataTag ||
        (sequence & kMaximumNegotiatedHeartbeatSequence) == 0U) {
        return std::nullopt;
    }
    const auto timeout =
        static_cast<std::uint16_t>((sequence >> kHeartbeatTimeoutShift) & kHeartbeatTimeoutMask);
    if (timeout < kMinimumWorkerIdleTimeoutSeconds || timeout > kMaximumWorkerIdleTimeoutSeconds) {
        return std::nullopt;
    }
    return timeout;
}

common::Result<std::vector<std::uint8_t>> encode_hello(const HelloMessage& message) {
    auto valid = validate_client_id(message.client_id);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    PayloadWriter writer;
    auto written = writer.write_string(message.client_id);
    if (!written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<HelloMessage> decode_hello(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto client_id = reader.read_string(kMaxProtocolIdentifierBytes);
    if (!client_id) {
        return common::Result<HelloMessage>::failure(client_id.error());
    }
    auto valid = validate_client_id(*client_id);
    if (!valid) {
        return common::Result<HelloMessage>::failure(valid.error());
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<HelloMessage>::failure(ended.error());
    }
    return HelloMessage{std::move(*client_id)};
}

common::Result<std::vector<std::uint8_t>> encode_hello_ack(const HelloAckMessage& message) {
    auto valid = validate_server_id(message.server_id);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    PayloadWriter writer;
    if (auto result = writer.write_string(message.server_id); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (auto result = writer.write_u64(std::bit_cast<std::uint64_t>(message.server_time_seconds));
        !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (auto result = writer.write_bytes(message.nonce); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    return std::move(writer).finish();
}

common::Result<HelloAckMessage> decode_hello_ack(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto server_id = reader.read_string(kMaxProtocolIdentifierBytes);
    if (!server_id) {
        return common::Result<HelloAckMessage>::failure(server_id.error());
    }
    auto valid = validate_server_id(*server_id);
    if (!valid) {
        return common::Result<HelloAckMessage>::failure(valid.error());
    }
    auto timestamp = reader.read_u64();
    if (!timestamp) {
        return common::Result<HelloAckMessage>::failure(timestamp.error());
    }
    auto nonce = read_fixed_bytes<kAuthenticationNonceSize>(reader);
    if (!nonce) {
        return common::Result<HelloAckMessage>::failure(nonce.error());
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<HelloAckMessage>::failure(ended.error());
    }
    return HelloAckMessage{std::move(*server_id), std::bit_cast<std::int64_t>(*timestamp),
                           std::move(*nonce)};
}

common::Result<std::vector<std::uint8_t>> encode_auth(const AuthMessage& message) {
    auto valid = validate_client_id(message.client_id);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    PayloadWriter writer;
    if (auto result = writer.write_string(message.client_id); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (auto result = writer.write_u64(std::bit_cast<std::uint64_t>(message.timestamp_seconds));
        !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (auto result = writer.write_bytes(message.nonce); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (auto result = writer.write_bytes(message.authentication_data); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    return std::move(writer).finish();
}

common::Result<AuthMessage> decode_auth(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto client_id = reader.read_string(kMaxProtocolIdentifierBytes);
    if (!client_id) {
        return common::Result<AuthMessage>::failure(client_id.error());
    }
    auto valid = validate_client_id(*client_id);
    if (!valid) {
        return common::Result<AuthMessage>::failure(valid.error());
    }
    auto timestamp = reader.read_u64();
    if (!timestamp) {
        return common::Result<AuthMessage>::failure(timestamp.error());
    }
    auto nonce = read_fixed_bytes<kAuthenticationNonceSize>(reader);
    if (!nonce) {
        return common::Result<AuthMessage>::failure(nonce.error());
    }
    auto authentication_data = read_fixed_bytes<kAuthenticationDataSize>(reader);
    if (!authentication_data) {
        return common::Result<AuthMessage>::failure(authentication_data.error());
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<AuthMessage>::failure(ended.error());
    }
    return AuthMessage{std::move(*client_id), std::bit_cast<std::int64_t>(*timestamp),
                       std::move(*nonce), std::move(*authentication_data)};
}

common::Result<std::vector<std::uint8_t>> encode_auth_ok(const AuthOkMessage& message) {
    auto valid = validate_auth_ok(message);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    PayloadWriter writer;
    if (auto result = writer.write_u64(message.session_generation); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (auto result = writer.write_u32(message.heartbeat_interval_milliseconds); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (auto result = writer.write_u16(message.min_idle_workers); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (auto result = writer.write_u16(message.max_idle_workers); !result) {
        return common::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    return std::move(writer).finish();
}

common::Result<AuthOkMessage> decode_auth_ok(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto generation = reader.read_u64();
    auto heartbeat = reader.read_u32();
    auto minimum_workers = reader.read_u16();
    auto maximum_workers = reader.read_u16();
    if (!generation || !heartbeat || !minimum_workers || !maximum_workers) {
        return common::Result<AuthOkMessage>::failure(common::ErrorCode::protocol_error,
                                                      "AUTH_OK payload is truncated");
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<AuthOkMessage>::failure(ended.error());
    }
    AuthOkMessage message{*generation, *heartbeat, *minimum_workers, *maximum_workers};
    auto valid = validate_auth_ok(message);
    if (!valid) {
        return common::Result<AuthOkMessage>::failure(valid.error());
    }
    return message;
}

common::Result<std::vector<std::uint8_t>> encode_auth_error(const AuthErrorMessage& message) {
    if (message.code == common::ErrorCode::ok) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::invalid_argument, "AUTH_ERROR must contain a failure code");
    }
    PayloadWriter writer;
    auto written = writer.write_string(common::to_string(message.code));
    if (!written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<AuthErrorMessage> decode_auth_error(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto code_text = reader.read_string(64U);
    if (!code_text) {
        return common::Result<AuthErrorMessage>::failure(code_text.error());
    }
    const auto code = common::error_code_from_string(*code_text);
    if (!code.has_value() || *code == common::ErrorCode::ok) {
        return common::Result<AuthErrorMessage>::failure(
            common::ErrorCode::protocol_error, "AUTH_ERROR contains an invalid error code");
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<AuthErrorMessage>::failure(ended.error());
    }
    return AuthErrorMessage{*code};
}

common::Result<std::vector<std::uint8_t>> encode_heartbeat(const HeartbeatMessage& message) {
    PayloadWriter writer;
    auto written = writer.write_u64(message.sequence);
    if (!written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<HeartbeatMessage> decode_heartbeat(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto sequence = reader.read_u64();
    if (!sequence) {
        return common::Result<HeartbeatMessage>::failure(sequence.error());
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<HeartbeatMessage>::failure(ended.error());
    }
    return HeartbeatMessage{*sequence};
}

common::Result<std::vector<std::uint8_t>>
encode_register_tunnel(const RegisterTunnelMessage& message) {
    auto valid = validate_tunnel_id(message.tunnel_id);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    if (message.bind_host.empty() || message.bind_host.size() > kMaxTunnelBindHostBytes ||
        message.bind_port == 0U) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::invalid_argument, "remote tunnel binding is invalid");
    }
    PayloadWriter writer;
    if (auto written = writer.write_string(message.tunnel_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    if (auto written = writer.write_string(message.bind_host); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    if (auto written = writer.write_u16(message.bind_port); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<RegisterTunnelMessage>
decode_register_tunnel(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto tunnel_id = reader.read_string(kMaxProtocolIdentifierBytes);
    auto bind_host = reader.read_string(kMaxTunnelBindHostBytes);
    auto bind_port = reader.read_u16();
    if (!tunnel_id || !bind_host || !bind_port) {
        return common::Result<RegisterTunnelMessage>::failure(
            common::ErrorCode::protocol_error, "REGISTER_TUNNEL payload is truncated");
    }
    auto valid = validate_tunnel_id(*tunnel_id);
    if (!valid) {
        return common::Result<RegisterTunnelMessage>::failure(valid.error());
    }
    if (bind_host->empty() || *bind_port == 0U) {
        return common::Result<RegisterTunnelMessage>::failure(common::ErrorCode::protocol_error,
                                                              "REGISTER_TUNNEL binding is invalid");
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<RegisterTunnelMessage>::failure(ended.error());
    }
    return RegisterTunnelMessage{std::move(*tunnel_id), std::move(*bind_host), *bind_port};
}

common::Result<std::vector<std::uint8_t>>
encode_register_tunnel_ok(const RegisterTunnelOkMessage& message) {
    return encode_tunnel_id(message.tunnel_id);
}

common::Result<RegisterTunnelOkMessage>
decode_register_tunnel_ok(const std::vector<std::uint8_t>& payload) {
    auto tunnel_id = decode_tunnel_id(payload);
    if (!tunnel_id) {
        return common::Result<RegisterTunnelOkMessage>::failure(tunnel_id.error());
    }
    return RegisterTunnelOkMessage{std::move(*tunnel_id)};
}

common::Result<std::vector<std::uint8_t>>
encode_register_tunnel_error(const RegisterTunnelErrorMessage& message) {
    auto valid = validate_tunnel_id(message.tunnel_id);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    if (message.code == common::ErrorCode::ok) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::invalid_argument, "REGISTER_TUNNEL_ERROR requires a failure code");
    }
    PayloadWriter writer;
    if (auto written = writer.write_string(message.tunnel_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    if (auto written = writer.write_string(common::to_string(message.code)); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<RegisterTunnelErrorMessage>
decode_register_tunnel_error(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto tunnel_id = reader.read_string(kMaxProtocolIdentifierBytes);
    auto code_text = reader.read_string(64U);
    if (!tunnel_id || !code_text) {
        return common::Result<RegisterTunnelErrorMessage>::failure(
            common::ErrorCode::protocol_error, "REGISTER_TUNNEL_ERROR payload is truncated");
    }
    auto valid = validate_tunnel_id(*tunnel_id);
    const auto code = common::error_code_from_string(*code_text);
    if (!valid || !code.has_value() || *code == common::ErrorCode::ok) {
        return common::Result<RegisterTunnelErrorMessage>::failure(
            common::ErrorCode::protocol_error, "REGISTER_TUNNEL_ERROR payload is invalid");
    }
    auto ended = reader.require_end();
    if (!ended) {
        return common::Result<RegisterTunnelErrorMessage>::failure(ended.error());
    }
    return RegisterTunnelErrorMessage{std::move(*tunnel_id), *code};
}

common::Result<std::vector<std::uint8_t>>
encode_unregister_tunnel(const UnregisterTunnelMessage& message) {
    return encode_tunnel_id(message.tunnel_id);
}

common::Result<UnregisterTunnelMessage>
decode_unregister_tunnel(const std::vector<std::uint8_t>& payload) {
    auto tunnel_id = decode_tunnel_id(payload);
    if (!tunnel_id) {
        return common::Result<UnregisterTunnelMessage>::failure(tunnel_id.error());
    }
    return UnregisterTunnelMessage{std::move(*tunnel_id)};
}

common::Result<std::vector<std::uint8_t>>
encode_unregister_tunnel_ok(const UnregisterTunnelOkMessage& message) {
    return encode_tunnel_id(message.tunnel_id);
}

common::Result<UnregisterTunnelOkMessage>
decode_unregister_tunnel_ok(const std::vector<std::uint8_t>& payload) {
    return decode_unregister_tunnel(payload);
}

common::Result<std::vector<std::uint8_t>>
encode_request_workers(const RequestWorkersMessage& message) {
    if (message.count == 0U || message.count > 128U) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::invalid_argument, "requested worker count is outside 1..128");
    }
    PayloadWriter writer;
    if (auto written = writer.write_u16(message.count); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<RequestWorkersMessage>
decode_request_workers(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto count = reader.read_u16();
    if (!count || *count == 0U || *count > 128U || !reader.require_end()) {
        return common::Result<RequestWorkersMessage>::failure(common::ErrorCode::protocol_error,
                                                              "REQUEST_WORKERS payload is invalid");
    }
    return RequestWorkersMessage{*count};
}

common::Result<std::vector<std::uint8_t>> encode_worker_hello(const WorkerHelloMessage& message) {
    auto client_valid = validate_client_id(message.client_id);
    auto worker_valid = validate_connection_id(message.worker_id);
    if (!client_valid || !worker_valid || message.session_generation == 0U) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::protocol_error, "WORKER_HELLO identity is invalid");
    }
    PayloadWriter writer;
    if (auto written = writer.write_string(message.client_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    if (auto written = writer.write_u64(message.session_generation); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    if (auto written = writer.write_string(message.worker_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<WorkerHelloMessage> decode_worker_hello(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto client_id = reader.read_string(kMaxProtocolIdentifierBytes);
    auto generation = reader.read_u64();
    auto worker_id = reader.read_string(kMaxProtocolIdentifierBytes);
    if (!client_id || !generation || !worker_id || *generation == 0U ||
        !validate_client_id(*client_id) || !validate_connection_id(*worker_id) ||
        !reader.require_end()) {
        return common::Result<WorkerHelloMessage>::failure(common::ErrorCode::protocol_error,
                                                           "WORKER_HELLO payload is invalid");
    }
    return WorkerHelloMessage{std::move(*client_id), *generation, std::move(*worker_id)};
}

common::Result<std::vector<std::uint8_t>>
encode_worker_accepted(const WorkerAcceptedMessage& message) {
    auto valid = validate_connection_id(message.worker_id);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    PayloadWriter writer;
    if (auto written = writer.write_string(message.worker_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<WorkerAcceptedMessage>
decode_worker_accepted(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto worker_id = reader.read_string(kMaxProtocolIdentifierBytes);
    if (!worker_id || !validate_connection_id(*worker_id) || !reader.require_end()) {
        return common::Result<WorkerAcceptedMessage>::failure(common::ErrorCode::protocol_error,
                                                              "WORKER_ACCEPTED payload is invalid");
    }
    return WorkerAcceptedMessage{std::move(*worker_id)};
}

common::Result<std::vector<std::uint8_t>> encode_start_relay(const StartRelayMessage& message) {
    if (!validate_tunnel_id(message.tunnel_id) || !validate_connection_id(message.connection_id)) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::protocol_error, "START_RELAY identity is invalid");
    }
    PayloadWriter writer;
    if (auto written = writer.write_string(message.tunnel_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    if (auto written = writer.write_string(message.connection_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<StartRelayMessage> decode_start_relay(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto tunnel_id = reader.read_string(kMaxProtocolIdentifierBytes);
    auto connection_id = reader.read_string(kMaxProtocolIdentifierBytes);
    if (!tunnel_id || !connection_id || !validate_tunnel_id(*tunnel_id) ||
        !validate_connection_id(*connection_id) || !reader.require_end()) {
        return common::Result<StartRelayMessage>::failure(common::ErrorCode::protocol_error,
                                                          "START_RELAY payload is invalid");
    }
    return StartRelayMessage{std::move(*tunnel_id), std::move(*connection_id)};
}

common::Result<std::vector<std::uint8_t>>
encode_local_connect_ok(const LocalConnectOkMessage& message) {
    auto valid = validate_connection_id(message.connection_id);
    if (!valid) {
        return common::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    PayloadWriter writer;
    if (auto written = writer.write_string(message.connection_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<LocalConnectOkMessage>
decode_local_connect_ok(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto connection_id = reader.read_string(kMaxProtocolIdentifierBytes);
    if (!connection_id || !validate_connection_id(*connection_id) || !reader.require_end()) {
        return common::Result<LocalConnectOkMessage>::failure(
            common::ErrorCode::protocol_error, "LOCAL_CONNECT_OK payload is invalid");
    }
    return LocalConnectOkMessage{std::move(*connection_id)};
}

common::Result<std::vector<std::uint8_t>>
encode_local_connect_error(const LocalConnectErrorMessage& message) {
    if (!validate_connection_id(message.connection_id) || message.code == common::ErrorCode::ok) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::protocol_error, "LOCAL_CONNECT_ERROR payload is invalid");
    }
    PayloadWriter writer;
    if (auto written = writer.write_string(message.connection_id); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    if (auto written = writer.write_string(common::to_string(message.code)); !written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<LocalConnectErrorMessage>
decode_local_connect_error(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto connection_id = reader.read_string(kMaxProtocolIdentifierBytes);
    auto code_text = reader.read_string(64U);
    const auto code = code_text ? common::error_code_from_string(*code_text) : std::nullopt;
    if (!connection_id || !validate_connection_id(*connection_id) || !code.has_value() ||
        *code == common::ErrorCode::ok || !reader.require_end()) {
        return common::Result<LocalConnectErrorMessage>::failure(
            common::ErrorCode::protocol_error, "LOCAL_CONNECT_ERROR payload is invalid");
    }
    return LocalConnectErrorMessage{std::move(*connection_id), *code};
}

} // namespace minitun::protocol
