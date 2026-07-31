#include <minitun/protocol/messages.hpp>

#include <bit>
#include <span>
#include <utility>

#include <minitun/common/id.hpp>
#include <minitun/protocol/codec.hpp>

namespace minitun::protocol {
namespace {

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
        return common::Result<void>::failure(
            common::ErrorCode::protocol_error,
            "heartbeat interval is outside the protocol limits");
    }
    if (message.min_idle_workers > message.max_idle_workers ||
        message.max_idle_workers > 128U) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "worker limits are inconsistent");
    }
    return common::Result<void>::success();
}

} // namespace

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

common::Result<std::vector<std::uint8_t>>
encode_hello_ack(const HelloAckMessage& message) {
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

common::Result<HelloAckMessage>
decode_hello_ack(const std::vector<std::uint8_t>& payload) {
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

common::Result<AuthOkMessage>
decode_auth_ok(const std::vector<std::uint8_t>& payload) {
    PayloadReader reader{payload};
    auto generation = reader.read_u64();
    auto heartbeat = reader.read_u32();
    auto minimum_workers = reader.read_u16();
    auto maximum_workers = reader.read_u16();
    if (!generation || !heartbeat || !minimum_workers || !maximum_workers) {
        return common::Result<AuthOkMessage>::failure(
            common::ErrorCode::protocol_error, "AUTH_OK payload is truncated");
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

common::Result<std::vector<std::uint8_t>>
encode_auth_error(const AuthErrorMessage& message) {
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

common::Result<AuthErrorMessage>
decode_auth_error(const std::vector<std::uint8_t>& payload) {
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

common::Result<std::vector<std::uint8_t>>
encode_heartbeat(const HeartbeatMessage& message) {
    PayloadWriter writer;
    auto written = writer.write_u64(message.sequence);
    if (!written) {
        return common::Result<std::vector<std::uint8_t>>::failure(written.error());
    }
    return std::move(writer).finish();
}

common::Result<HeartbeatMessage>
decode_heartbeat(const std::vector<std::uint8_t>& payload) {
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

} // namespace minitun::protocol
