#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/protocol/codec.hpp>
#include <minitun/protocol/messages.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::string generated_id(const common::IdKind kind) {
    auto id = common::Id::generate(kind);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

template <typename Decoder>
void expect_all_truncations_and_trailing_bytes(const std::vector<std::uint8_t>& payload,
                                               Decoder&& decoder) {
    ASSERT_FALSE(payload.empty());
    for (std::size_t size = 0U; size < payload.size(); ++size) {
        SCOPED_TRACE(size);
        const std::vector<std::uint8_t> prefix(payload.begin(),
                                               payload.begin() + static_cast<std::ptrdiff_t>(size));
        EXPECT_FALSE(decoder(prefix));
    }
    auto trailing = payload;
    trailing.push_back(0U);
    EXPECT_FALSE(decoder(trailing));
}

template <typename WriteFields>
[[nodiscard]] std::vector<std::uint8_t> wire_payload(WriteFields&& write_fields) {
    PayloadWriter writer;
    if (!write_fields(writer)) {
        throw std::runtime_error("failed to construct a malformed test payload");
    }
    auto payload = std::move(writer).finish();
    if (!payload) {
        throw std::runtime_error("failed to finish a malformed test payload");
    }
    return std::move(*payload);
}

TEST(RemoteMessagesTest, RoundTripsHandshakeAndHeartbeatPayloads) {
    AuthenticationNonce nonce{};
    AuthenticationData digest{};
    for (std::size_t index = 0U; index < nonce.size(); ++index) {
        nonce[index] = static_cast<std::uint8_t>(index);
        digest[index] = static_cast<std::uint8_t>(0xffU - index);
    }

    const HelloMessage hello{generated_id(common::IdKind::client), kSupportedCapabilities};
    const HelloAckMessage ack{generated_id(common::IdKind::server), -123, nonce,
                              kRequiredCapabilities};
    const AuthMessage auth{hello.client_id, 456, nonce, digest, kRequiredCapabilities};
    const AuthOkMessage ok{99U, 5'000U, 2U, 32U};
    const AuthErrorMessage error{common::ErrorCode::authentication_failed};
    const HeartbeatMessage heartbeat{0x0102030405060708ULL};

    auto hello_payload = encode_hello(hello);
    auto ack_payload = encode_hello_ack(ack);
    auto auth_payload = encode_auth(auth);
    auto ok_payload = encode_auth_ok(ok);
    auto error_payload = encode_auth_error(error);
    auto heartbeat_payload = encode_heartbeat(heartbeat);
    ASSERT_TRUE(hello_payload);
    ASSERT_TRUE(ack_payload);
    ASSERT_TRUE(auth_payload);
    ASSERT_TRUE(ok_payload);
    ASSERT_TRUE(error_payload);
    ASSERT_TRUE(heartbeat_payload);

    EXPECT_EQ(*decode_hello(*hello_payload), hello);
    EXPECT_EQ(*decode_hello_ack(*ack_payload), ack);
    EXPECT_EQ(*decode_auth(*auth_payload), auth);
    EXPECT_EQ(*decode_auth_ok(*ok_payload), ok);
    EXPECT_EQ(*decode_auth_error(*error_payload), error);
    EXPECT_EQ(*decode_heartbeat(*heartbeat_payload), heartbeat);
}

TEST(RemoteMessagesTest, RejectsInvalidIdentifiersFixedLengthsAndTrailingBytes) {
    const auto invalid_hello = encode_hello({"client_not-an-id"});
    ASSERT_FALSE(invalid_hello);
    EXPECT_EQ(invalid_hello.error().code(), common::ErrorCode::protocol_error);

    const HelloAckMessage ack{generated_id(common::IdKind::server), 0, {}};
    auto ack_payload = encode_hello_ack(ack);
    ASSERT_TRUE(ack_payload);
    ASSERT_FALSE(ack_payload->empty());
    ack_payload->pop_back();
    EXPECT_FALSE(decode_hello_ack(*ack_payload));

    auto heartbeat = encode_heartbeat({1U});
    ASSERT_TRUE(heartbeat);
    heartbeat->push_back(0U);
    const auto trailing = decode_heartbeat(*heartbeat);
    ASSERT_FALSE(trailing);
    EXPECT_EQ(trailing.error().code(), common::ErrorCode::protocol_error);
}

TEST(RemoteMessagesTest, EnforcesAuthOkBoundsAndFailureCodes) {
    EXPECT_FALSE(encode_auth_ok({0U, 5'000U, 2U, 32U}));
    EXPECT_FALSE(encode_auth_ok({1U, 999U, 2U, 32U}));
    EXPECT_FALSE(encode_auth_ok({1U, 5'000U, 33U, 32U}));
    EXPECT_FALSE(encode_auth_ok({1U, 5'000U, 2U, 129U}));
    EXPECT_FALSE(encode_auth_error({common::ErrorCode::ok}));
}

TEST(RemoteMessagesTest, NegotiatesWorkerIdleTimeoutThroughOpaqueHeartbeatSequence) {
    const auto encoded = encode_worker_timeout_heartbeat_sequence(42U, 300U);
    ASSERT_TRUE(encoded) << encoded.error();
    EXPECT_NE(*encoded, 42U);
    EXPECT_EQ(decode_worker_idle_timeout_seconds(*encoded), 300U);
    EXPECT_FALSE(decode_worker_idle_timeout_seconds(42U).has_value());

    EXPECT_FALSE(encode_worker_timeout_heartbeat_sequence(0U, 60U));
    EXPECT_FALSE(
        encode_worker_timeout_heartbeat_sequence(kMaximumNegotiatedHeartbeatSequence + 1U, 60U));
    EXPECT_FALSE(encode_worker_timeout_heartbeat_sequence(1U, 0U));
    EXPECT_FALSE(encode_worker_timeout_heartbeat_sequence(1U, 301U));
}

TEST(RemoteMessagesTest, RoundTripsTunnelRegistrationPayloads) {
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const RegisterTunnelMessage registration{tunnel_id, "0.0.0.0", 6'000U, 7U};
    const RegisterTunnelOkMessage registered{tunnel_id, 7U};
    const RegisterTunnelErrorMessage rejected{tunnel_id, common::ErrorCode::remote_port_in_use, 7U};
    const UnregisterTunnelMessage removal{tunnel_id, 7U};

    const auto registration_payload = encode_register_tunnel(registration);
    const auto registered_payload = encode_register_tunnel_ok(registered);
    const auto rejected_payload = encode_register_tunnel_error(rejected);
    const auto removal_payload = encode_unregister_tunnel(removal);
    const auto removed_payload = encode_unregister_tunnel_ok(removal);
    ASSERT_TRUE(registration_payload);
    ASSERT_TRUE(registered_payload);
    ASSERT_TRUE(rejected_payload);
    ASSERT_TRUE(removal_payload);
    ASSERT_TRUE(removed_payload);

    EXPECT_EQ(*decode_register_tunnel(*registration_payload), registration);
    EXPECT_EQ(*decode_register_tunnel_ok(*registered_payload), registered);
    EXPECT_EQ(*decode_register_tunnel_error(*rejected_payload), rejected);
    EXPECT_EQ(*decode_unregister_tunnel(*removal_payload), removal);
    EXPECT_EQ(*decode_unregister_tunnel_ok(*removed_payload), removal);
}

TEST(RemoteMessagesTest, NegotiatesTransportModesWithoutChangingTheTcpWireImage) {
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const std::string connection_id = generated_id(common::IdKind::connection);
    const RegisterTunnelMessage tcp_registration{tunnel_id, "0.0.0.0", 6'000U, 7U, TunnelMode::tcp};
    const StartRelayMessage tcp_relay{tunnel_id, connection_id, TunnelMode::tcp};
    const auto tcp_registration_payload = encode_register_tunnel(tcp_registration);
    const auto tcp_relay_payload = encode_start_relay(tcp_relay);
    ASSERT_TRUE(tcp_registration_payload);
    ASSERT_TRUE(tcp_relay_payload);

    for (const auto mode : {TunnelMode::udp, TunnelMode::socks5, TunnelMode::p2p}) {
        auto registration = tcp_registration;
        registration.mode = mode;
        auto relay = tcp_relay;
        relay.mode = mode;
        const auto registration_payload = encode_register_tunnel(registration);
        const auto relay_payload = encode_start_relay(relay);
        ASSERT_TRUE(registration_payload);
        ASSERT_TRUE(relay_payload);
        ASSERT_EQ(registration_payload->size(), tcp_registration_payload->size() + 1U);
        ASSERT_EQ(relay_payload->size(), tcp_relay_payload->size() + 1U);
        EXPECT_EQ(*decode_register_tunnel(*registration_payload), registration);
        EXPECT_EQ(*decode_start_relay(*relay_payload), relay);
        EXPECT_TRUE(supports_tunnel_mode(kSupportedCapabilities, mode));
        EXPECT_FALSE(supports_tunnel_mode(kRequiredCapabilities, mode));
    }
    EXPECT_TRUE(supports_tunnel_mode(kRequiredCapabilities, TunnelMode::tcp));
}

TEST(RemoteMessagesTest, RejectsInvalidTunnelRegistrationPayloads) {
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    EXPECT_FALSE(encode_register_tunnel({"tun_invalid", "0.0.0.0", 6'000U}));
    EXPECT_FALSE(encode_register_tunnel({tunnel_id, "", 6'000U}));
    EXPECT_FALSE(encode_register_tunnel({tunnel_id, "0.0.0.0", 0U}));
    EXPECT_FALSE(encode_register_tunnel({tunnel_id, "0.0.0.0", 6'000U, 0U}));
    EXPECT_FALSE(encode_register_tunnel_error({tunnel_id, common::ErrorCode::ok}));

    auto registration = encode_register_tunnel({tunnel_id, "0.0.0.0", 6'000U});
    ASSERT_TRUE(registration);
    registration->push_back(0U);
    EXPECT_FALSE(decode_register_tunnel(*registration));
}

TEST(RemoteMessagesTest, RoundTripsWorkerPoolPayloads) {
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string worker_id = generated_id(common::IdKind::connection);
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const std::string connection_id = generated_id(common::IdKind::connection);

    const RequestWorkersMessage request{2U};
    AuthenticationNonce worker_nonce{};
    worker_nonce.fill(0x21U);
    AuthenticationData worker_proof{};
    worker_proof.fill(0x43U);
    const WorkerHelloMessage hello{client_id, 42U,          worker_id,
                                   1'234'567, worker_nonce, worker_proof};
    const WorkerAcceptedMessage accepted{worker_id};
    const StartRelayMessage relay{tunnel_id, connection_id};
    const LocalConnectOkMessage connected{connection_id};
    const LocalConnectErrorMessage failed{connection_id, common::ErrorCode::local_connect_failed};

    const auto request_payload = encode_request_workers(request);
    const auto hello_payload = encode_worker_hello(hello);
    const auto accepted_payload = encode_worker_accepted(accepted);
    const auto relay_payload = encode_start_relay(relay);
    const auto connected_payload = encode_local_connect_ok(connected);
    const auto failed_payload = encode_local_connect_error(failed);
    ASSERT_TRUE(request_payload);
    ASSERT_TRUE(hello_payload);
    ASSERT_TRUE(accepted_payload);
    ASSERT_TRUE(relay_payload);
    ASSERT_TRUE(connected_payload);
    ASSERT_TRUE(failed_payload);

    EXPECT_EQ(*decode_request_workers(*request_payload), request);
    EXPECT_EQ(*decode_worker_hello(*hello_payload), hello);
    EXPECT_EQ(*decode_worker_accepted(*accepted_payload), accepted);
    EXPECT_EQ(*decode_start_relay(*relay_payload), relay);
    EXPECT_EQ(*decode_local_connect_ok(*connected_payload), connected);
    EXPECT_EQ(*decode_local_connect_error(*failed_payload), failed);
}

TEST(RemoteMessagesTest, RejectsInvalidWorkerPoolPayloads) {
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string worker_id = generated_id(common::IdKind::connection);
    const std::string connection_id = generated_id(common::IdKind::connection);

    EXPECT_FALSE(encode_request_workers({0U}));
    EXPECT_FALSE(encode_request_workers({129U}));
    EXPECT_FALSE(encode_worker_hello({client_id, 0U, worker_id}));
    EXPECT_FALSE(encode_worker_accepted({"conn_invalid"}));
    EXPECT_FALSE(encode_start_relay({"tun_invalid", connection_id}));
    EXPECT_FALSE(encode_local_connect_error({connection_id, common::ErrorCode::ok}));
}

TEST(RemoteMessagesTest, EveryDecoderRejectsAllTruncationsAndTrailingBytes) {
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string server_id = generated_id(common::IdKind::server);
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const std::string connection_id = generated_id(common::IdKind::connection);
    AuthenticationNonce nonce{};
    AuthenticationData digest{};
    nonce.fill(0x31U);
    digest.fill(0x71U);

    const auto hello = encode_hello({client_id, kSupportedCapabilities});
    const auto ack = encode_hello_ack({server_id, -17, nonce, kRequiredCapabilities});
    const auto auth = encode_auth({client_id, 23, nonce, digest, kRequiredCapabilities});
    const auto auth_ok = encode_auth_ok({7U, 5'000U, 1U, 8U});
    const auto auth_error = encode_auth_error({common::ErrorCode::authentication_failed});
    const auto heartbeat = encode_heartbeat({9U});
    const auto registration = encode_register_tunnel({tunnel_id, "0.0.0.0", 6'000U, 11U});
    const auto registration_ok = encode_register_tunnel_ok({tunnel_id, 11U});
    const auto registration_error =
        encode_register_tunnel_error({tunnel_id, common::ErrorCode::remote_port_in_use, 11U});
    const auto unregistration = encode_unregister_tunnel({tunnel_id, 12U});
    const auto unregistration_ok = encode_unregister_tunnel_ok({tunnel_id, 12U});
    const auto request_workers = encode_request_workers({4U});
    const auto worker_hello =
        encode_worker_hello({client_id, 7U, connection_id, -99, nonce, digest});
    const auto worker_accepted = encode_worker_accepted({connection_id});
    const auto relay = encode_start_relay({tunnel_id, connection_id});
    const auto local_ok = encode_local_connect_ok({connection_id});
    const auto local_error =
        encode_local_connect_error({connection_id, common::ErrorCode::local_connect_failed});
    ASSERT_TRUE(hello);
    ASSERT_TRUE(ack);
    ASSERT_TRUE(auth);
    ASSERT_TRUE(auth_ok);
    ASSERT_TRUE(auth_error);
    ASSERT_TRUE(heartbeat);
    ASSERT_TRUE(registration);
    ASSERT_TRUE(registration_ok);
    ASSERT_TRUE(registration_error);
    ASSERT_TRUE(unregistration);
    ASSERT_TRUE(unregistration_ok);
    ASSERT_TRUE(request_workers);
    ASSERT_TRUE(worker_hello);
    ASSERT_TRUE(worker_accepted);
    ASSERT_TRUE(relay);
    ASSERT_TRUE(local_ok);
    ASSERT_TRUE(local_error);

    expect_all_truncations_and_trailing_bytes(*hello, decode_hello);
    expect_all_truncations_and_trailing_bytes(*ack, decode_hello_ack);
    expect_all_truncations_and_trailing_bytes(*auth, decode_auth);
    expect_all_truncations_and_trailing_bytes(*auth_ok, decode_auth_ok);
    expect_all_truncations_and_trailing_bytes(*auth_error, decode_auth_error);
    expect_all_truncations_and_trailing_bytes(*heartbeat, decode_heartbeat);
    expect_all_truncations_and_trailing_bytes(*registration, decode_register_tunnel);
    expect_all_truncations_and_trailing_bytes(*registration_ok, decode_register_tunnel_ok);
    expect_all_truncations_and_trailing_bytes(*registration_error, decode_register_tunnel_error);
    expect_all_truncations_and_trailing_bytes(*unregistration, decode_unregister_tunnel);
    expect_all_truncations_and_trailing_bytes(*unregistration_ok, decode_unregister_tunnel_ok);
    expect_all_truncations_and_trailing_bytes(*request_workers, decode_request_workers);
    expect_all_truncations_and_trailing_bytes(*worker_hello, decode_worker_hello);
    expect_all_truncations_and_trailing_bytes(*worker_accepted, decode_worker_accepted);
    expect_all_truncations_and_trailing_bytes(*relay, decode_start_relay);
    expect_all_truncations_and_trailing_bytes(*local_ok, decode_local_connect_ok);
    expect_all_truncations_and_trailing_bytes(*local_error, decode_local_connect_error);
}

TEST(RemoteMessagesTest, RejectsEverySemanticWireInvariant) {
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string server_id = generated_id(common::IdKind::server);
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const std::string connection_id = generated_id(common::IdKind::connection);
    AuthenticationNonce nonce{};
    AuthenticationData digest{};

    const auto hello_wire = [](const std::string_view id, const CapabilitySet capabilities) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(id) && writer.write_u64(capabilities);
        });
    };
    EXPECT_FALSE(decode_hello(hello_wire("client_invalid", kRequiredCapabilities)));
    EXPECT_FALSE(decode_hello(hello_wire(client_id, 0U)));
    EXPECT_FALSE(encode_hello({client_id, 0U}));
    EXPECT_TRUE(encode_hello({client_id, kRequiredCapabilities | (1ULL << 63U)}));

    const auto ack_wire = [&nonce](const std::string_view id, const CapabilitySet capabilities) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(id) && writer.write_u64(0U) && writer.write_bytes(nonce) &&
                   writer.write_u64(capabilities);
        });
    };
    EXPECT_FALSE(decode_hello_ack(ack_wire("server_invalid", kRequiredCapabilities)));
    EXPECT_FALSE(decode_hello_ack(ack_wire(server_id, 0U)));
    EXPECT_FALSE(decode_hello_ack(ack_wire(server_id, kRequiredCapabilities | (1ULL << 63U))));
    EXPECT_FALSE(encode_hello_ack({server_id, 0, nonce, 0U}));
    EXPECT_FALSE(encode_hello_ack({server_id, 0, nonce, kRequiredCapabilities | (1ULL << 63U)}));

    const auto auth_wire = [&nonce, &digest](const std::string_view id,
                                             const CapabilitySet capabilities) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(id) && writer.write_u64(0U) && writer.write_bytes(nonce) &&
                   writer.write_bytes(digest) && writer.write_u64(capabilities);
        });
    };
    EXPECT_FALSE(decode_auth(auth_wire("client_invalid", kRequiredCapabilities)));
    EXPECT_FALSE(decode_auth(auth_wire(client_id, 0U)));
    EXPECT_FALSE(decode_auth(auth_wire(client_id, kRequiredCapabilities | (1ULL << 63U))));
    EXPECT_FALSE(encode_auth({"client_invalid", 0, nonce, digest, kRequiredCapabilities}));
    EXPECT_FALSE(encode_auth({client_id, 0, nonce, digest, 0U}));

    const auto auth_ok_wire = [](const std::uint64_t generation, const std::uint32_t heartbeat,
                                 const std::uint16_t minimum, const std::uint16_t maximum) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_u64(generation) && writer.write_u32(heartbeat) &&
                   writer.write_u16(minimum) && writer.write_u16(maximum);
        });
    };
    EXPECT_FALSE(decode_auth_ok(auth_ok_wire(0U, 5'000U, 1U, 8U)));
    EXPECT_FALSE(decode_auth_ok(auth_ok_wire(1U, 999U, 1U, 8U)));
    EXPECT_FALSE(decode_auth_ok(auth_ok_wire(1U, 60'001U, 1U, 8U)));
    EXPECT_FALSE(decode_auth_ok(auth_ok_wire(1U, 5'000U, 9U, 8U)));
    EXPECT_FALSE(decode_auth_ok(auth_ok_wire(1U, 5'000U, 1U, 129U)));

    const auto error_wire = [](const std::string_view code) {
        return wire_payload([&](PayloadWriter& writer) { return writer.write_string(code); });
    };
    EXPECT_FALSE(decode_auth_error(error_wire("unknown")));
    EXPECT_FALSE(decode_auth_error(error_wire("ok")));

    const auto tunnel_revision_wire = [](const std::string_view id, const std::uint64_t revision) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(id) && writer.write_u64(revision);
        });
    };
    EXPECT_FALSE(decode_register_tunnel_ok(tunnel_revision_wire("tun_invalid", 1U)));
    EXPECT_FALSE(decode_unregister_tunnel(tunnel_revision_wire(tunnel_id, 0U)));
    EXPECT_FALSE(encode_register_tunnel_ok({tunnel_id, 0U}));
    EXPECT_FALSE(encode_unregister_tunnel({"tun_invalid", 1U}));
    EXPECT_FALSE(encode_unregister_tunnel_ok({tunnel_id, 0U}));

    const auto register_wire = [](const std::string_view id, const std::string_view host,
                                  const std::uint16_t port, const std::uint64_t revision) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(id) && writer.write_string(host) && writer.write_u16(port) &&
                   writer.write_u64(revision);
        });
    };
    EXPECT_FALSE(decode_register_tunnel(register_wire("tun_invalid", "0.0.0.0", 1U, 1U)));
    EXPECT_FALSE(decode_register_tunnel(register_wire(tunnel_id, "", 1U, 1U)));
    EXPECT_FALSE(decode_register_tunnel(register_wire(tunnel_id, "0.0.0.0", 0U, 1U)));
    EXPECT_FALSE(decode_register_tunnel(register_wire(tunnel_id, "0.0.0.0", 1U, 0U)));
    EXPECT_FALSE(encode_register_tunnel(
        {tunnel_id, std::string(kMaxTunnelBindHostBytes + 1U, 'x'), 1U, 1U}));

    const auto register_error_wire = [](const std::string_view id, const std::string_view code,
                                        const std::uint64_t revision) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(id) && writer.write_string(code) &&
                   writer.write_u64(revision);
        });
    };
    EXPECT_FALSE(
        decode_register_tunnel_error(register_error_wire("tun_invalid", "remote_port_in_use", 1U)));
    EXPECT_FALSE(decode_register_tunnel_error(register_error_wire(tunnel_id, "unknown", 1U)));
    EXPECT_FALSE(decode_register_tunnel_error(register_error_wire(tunnel_id, "ok", 1U)));
    EXPECT_FALSE(
        decode_register_tunnel_error(register_error_wire(tunnel_id, "remote_port_in_use", 0U)));
    EXPECT_FALSE(
        encode_register_tunnel_error({"tun_invalid", common::ErrorCode::remote_port_in_use, 1U}));
    EXPECT_FALSE(
        encode_register_tunnel_error({tunnel_id, common::ErrorCode::remote_port_in_use, 0U}));

    const auto workers_wire = [](const std::uint16_t count) {
        return wire_payload([&](PayloadWriter& writer) { return writer.write_u16(count); });
    };
    EXPECT_FALSE(decode_request_workers(workers_wire(0U)));
    EXPECT_FALSE(decode_request_workers(workers_wire(129U)));

    const auto worker_wire = [&nonce, &digest](const std::string_view client,
                                               const std::uint64_t generation,
                                               const std::string_view worker) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(client) && writer.write_u64(generation) &&
                   writer.write_string(worker) && writer.write_u64(0U) &&
                   writer.write_bytes(nonce) && writer.write_bytes(digest);
        });
    };
    EXPECT_FALSE(decode_worker_hello(worker_wire("client_invalid", 1U, connection_id)));
    EXPECT_FALSE(decode_worker_hello(worker_wire(client_id, 0U, connection_id)));
    EXPECT_FALSE(decode_worker_hello(worker_wire(client_id, 1U, "conn_invalid")));
    EXPECT_FALSE(encode_worker_hello({"client_invalid", 1U, connection_id}));
    EXPECT_FALSE(encode_worker_hello({client_id, 1U, "conn_invalid"}));

    const auto identity_wire = [](const std::string_view identity) {
        return wire_payload([&](PayloadWriter& writer) { return writer.write_string(identity); });
    };
    EXPECT_FALSE(decode_worker_accepted(identity_wire("conn_invalid")));
    EXPECT_FALSE(decode_local_connect_ok(identity_wire("conn_invalid")));
    EXPECT_FALSE(encode_local_connect_ok({"conn_invalid"}));

    const auto relay_wire = [](const std::string_view tunnel, const std::string_view connection) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(tunnel) && writer.write_string(connection);
        });
    };
    EXPECT_FALSE(decode_start_relay(relay_wire("tun_invalid", connection_id)));
    EXPECT_FALSE(decode_start_relay(relay_wire(tunnel_id, "conn_invalid")));
    EXPECT_FALSE(encode_start_relay({tunnel_id, "conn_invalid"}));

    const auto local_error_wire = [](const std::string_view connection,
                                     const std::string_view code) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(connection) && writer.write_string(code);
        });
    };
    EXPECT_FALSE(decode_local_connect_error(local_error_wire("conn_invalid", "timeout")));
    EXPECT_FALSE(decode_local_connect_error(local_error_wire(connection_id, "unknown")));
    EXPECT_FALSE(decode_local_connect_error(local_error_wire(connection_id, "ok")));
    EXPECT_FALSE(
        encode_local_connect_error({"conn_invalid", common::ErrorCode::local_connect_failed}));

    const auto tagged = encode_worker_timeout_heartbeat_sequence(1U, 1U);
    ASSERT_TRUE(tagged);
    constexpr std::uint64_t timeout_bits = 0x1ffULL << 39U;
    EXPECT_FALSE(decode_worker_idle_timeout_seconds(*tagged & ~timeout_bits).has_value());
}

TEST(RemoteMessagesTest, RoundTripsStartRelaySourceEndpointExtension) {
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const std::string connection_id = generated_id(common::IdKind::connection);
    const StartRelayMessage relay{tunnel_id, connection_id, TunnelMode::tcp, "198.51.100.7", 4'321U};
    const auto payload = encode_start_relay(relay);
    ASSERT_TRUE(payload) << payload.error();
    const auto decoded = decode_start_relay(*payload);
    ASSERT_TRUE(decoded) << decoded.error();
    EXPECT_EQ(*decoded, relay);

    const StartRelayMessage without_source{tunnel_id, connection_id, TunnelMode::udp};
    const auto plain = encode_start_relay(without_source);
    ASSERT_TRUE(plain) << plain.error();
    EXPECT_EQ(*decode_start_relay(*plain), without_source);

    const StartRelayMessage udp_source{tunnel_id, connection_id, TunnelMode::udp,
                                       "2001:db8::42", 53'321U};
    const auto udp_extended = encode_start_relay(udp_source);
    ASSERT_TRUE(udp_extended) << udp_extended.error();
    EXPECT_EQ(*decode_start_relay(*udp_extended), udp_source);

    // The legacy TCP image is a valid strict prefix of the extended image, so
    // the generic truncation helper cannot apply; truncate the extension only.
    const auto legacy = encode_start_relay({tunnel_id, connection_id, TunnelMode::tcp});
    ASSERT_TRUE(legacy) << legacy.error();
    for (std::size_t size = legacy->size() + 1U; size < payload->size(); ++size) {
        SCOPED_TRACE(size);
        const std::vector<std::uint8_t> prefix(
            payload->begin(), payload->begin() + static_cast<std::ptrdiff_t>(size));
        EXPECT_FALSE(decode_start_relay(prefix));
    }
    auto trailing = *payload;
    trailing.push_back(0U);
    EXPECT_FALSE(decode_start_relay(trailing));

    EXPECT_FALSE(encode_start_relay(
        {tunnel_id, connection_id, TunnelMode::tcp, "198.51.100.7", std::nullopt}));
    EXPECT_FALSE(encode_start_relay(
        {tunnel_id, connection_id, TunnelMode::tcp, std::nullopt, 4'321U}));

    const auto reject = [&](const std::uint8_t mode, const std::string_view host,
                            const std::uint16_t port) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(tunnel_id) && writer.write_string(connection_id) &&
                   writer.write_u8(mode) && writer.write_string(host) &&
                   writer.write_u16(port);
        });
    };
    EXPECT_TRUE(decode_start_relay(reject(0U, "198.51.100.7", 4'321U)));
    EXPECT_FALSE(decode_start_relay(reject(0U, "198.51.100.7", 0U)));
    EXPECT_FALSE(decode_start_relay(reject(0U, "not-an-address", 4'321U)));
    EXPECT_FALSE(decode_start_relay(reject(255U, "198.51.100.7", 4'321U)));
}

TEST(RemoteMessagesTest, RejectsExplicitTcpAndUnknownTransportModeExtensions) {
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const std::string connection_id = generated_id(common::IdKind::connection);

    const auto register_with_mode = [&tunnel_id](const std::uint8_t mode) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(tunnel_id) && writer.write_string("0.0.0.0") &&
                   writer.write_u16(1U) && writer.write_u64(1U) && writer.write_u8(mode);
        });
    };
    EXPECT_FALSE(decode_register_tunnel(register_with_mode(0U)));
    EXPECT_FALSE(decode_register_tunnel(register_with_mode(255U)));
    const auto register_double_extension = wire_payload([&](PayloadWriter& writer) {
        return writer.write_string(tunnel_id) && writer.write_string("0.0.0.0") &&
               writer.write_u16(1U) && writer.write_u64(1U) && writer.write_u8(1U) &&
               writer.write_u8(2U);
    });
    EXPECT_FALSE(decode_register_tunnel(register_double_extension));

    const auto relay_with_mode = [&](const std::uint8_t mode) {
        return wire_payload([&](PayloadWriter& writer) {
            return writer.write_string(tunnel_id) && writer.write_string(connection_id) &&
                   writer.write_u8(mode);
        });
    };
    EXPECT_FALSE(decode_start_relay(relay_with_mode(0U)));
    EXPECT_FALSE(decode_start_relay(relay_with_mode(255U)));
    const auto relay_double_extension = wire_payload([&](PayloadWriter& writer) {
        return writer.write_string(tunnel_id) && writer.write_string(connection_id) &&
               writer.write_u8(1U) && writer.write_u8(2U);
    });
    EXPECT_FALSE(decode_start_relay(relay_double_extension));

    const auto workers_wire = [](const std::uint16_t count) {
        return wire_payload([&](PayloadWriter& writer) { return writer.write_u16(count); });
    };
    EXPECT_FALSE(decode_request_workers(workers_wire(0U)));
    EXPECT_FALSE(decode_request_workers(workers_wire(129U)));
}

template <typename Message>
void expect_per_field_inequalities(const Message& base,
                                   const std::initializer_list<Message> mutated) {
    EXPECT_EQ(base, base);
    for (const auto& other : mutated) {
        EXPECT_NE(base, other);
    }
}

TEST(RemoteMessagesTest, PerFieldInequalityCoversEveryMessageComparison) {
    const std::string client_id = generated_id(common::IdKind::client);
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    const std::string connection_id = "conn_00000000000000000000000000000001";
    const std::string worker_id = "worker_00000000000000000000000000000001";
    const std::string server_id = "server_00000000000000000000000000000001";

    {
        HelloMessage base{.client_id = client_id};
        expect_per_field_inequalities(
            base,
            {HelloMessage{.client_id = "other"},
             HelloMessage{.client_id = client_id,
                          .capabilities = capability_bit(Capability::client_certificate_binding)}});
    }
    {
        HelloAckMessage base{.server_id = server_id};
        expect_per_field_inequalities(
            base, {HelloAckMessage{.server_id = "other"},
                   HelloAckMessage{.server_id = server_id, .server_time_seconds = 1},
                   [&] {
                       HelloAckMessage value{.server_id = server_id};
                       value.nonce[0] = 1U;
                       return value;
                   }(),
                   HelloAckMessage{.server_id = server_id,
                                   .selected_capabilities =
                                       capability_bit(Capability::tunnel_revisions)}});
    }
    {
        AuthMessage base{.client_id = client_id};
        expect_per_field_inequalities(
            base,
            {AuthMessage{.client_id = "other"},
             AuthMessage{.client_id = client_id, .timestamp_seconds = 1},
             [&] {
                 AuthMessage value{.client_id = client_id};
                 value.nonce[0] = 1U;
                 return value;
             }(),
             [&] {
                 AuthMessage value{.client_id = client_id};
                 value.authentication_data[0] = 1U;
                 return value;
             }(),
             AuthMessage{.client_id = client_id,
                         .selected_capabilities = capability_bit(Capability::tunnel_revisions)}});
    }
    {
        AuthOkMessage base{};
        expect_per_field_inequalities(base, {AuthOkMessage{.session_generation = 1U},
                                             AuthOkMessage{.heartbeat_interval_milliseconds = 1U},
                                             AuthOkMessage{.min_idle_workers = 1U},
                                             AuthOkMessage{.max_idle_workers = 1U}});
    }
    {
        AuthErrorMessage base{};
        expect_per_field_inequalities(base,
                                      {AuthErrorMessage{.code = common::ErrorCode::tls_error}});
    }
    {
        HeartbeatMessage base{};
        expect_per_field_inequalities(base, {HeartbeatMessage{.sequence = 1U}});
    }
    {
        RegisterTunnelMessage base{.tunnel_id = tunnel_id, .bind_host = "0.0.0.0"};
        expect_per_field_inequalities(
            base,
            {RegisterTunnelMessage{.tunnel_id = "other", .bind_host = "0.0.0.0"},
             RegisterTunnelMessage{.tunnel_id = tunnel_id, .bind_host = "other"},
             RegisterTunnelMessage{.tunnel_id = tunnel_id, .bind_host = "0.0.0.0", .bind_port = 1U},
             RegisterTunnelMessage{
                 .tunnel_id = tunnel_id, .bind_host = "0.0.0.0", .desired_revision = 2U},
             RegisterTunnelMessage{
                 .tunnel_id = tunnel_id, .bind_host = "0.0.0.0", .mode = TunnelMode::udp}});
    }
    {
        RegisterTunnelOkMessage base{.tunnel_id = tunnel_id};
        expect_per_field_inequalities(
            base, {RegisterTunnelOkMessage{.tunnel_id = "other"},
                   RegisterTunnelOkMessage{.tunnel_id = tunnel_id, .desired_revision = 2U}});
    }
    {
        RegisterTunnelErrorMessage base{.tunnel_id = tunnel_id};
        expect_per_field_inequalities(
            base, {RegisterTunnelErrorMessage{.tunnel_id = "other"},
                   RegisterTunnelErrorMessage{.tunnel_id = tunnel_id,
                                              .code = common::ErrorCode::remote_port_in_use},
                   RegisterTunnelErrorMessage{.tunnel_id = tunnel_id, .desired_revision = 2U}});
    }
    {
        UnregisterTunnelMessage base{.tunnel_id = tunnel_id};
        expect_per_field_inequalities(
            base, {UnregisterTunnelMessage{.tunnel_id = "other"},
                   UnregisterTunnelMessage{.tunnel_id = tunnel_id, .desired_revision = 2U}});
    }
    {
        RequestWorkersMessage base{};
        expect_per_field_inequalities(base, {RequestWorkersMessage{.count = 1U}});
    }
    {
        WorkerHelloMessage base{.client_id = client_id, .worker_id = worker_id};
        expect_per_field_inequalities(
            base, {WorkerHelloMessage{.client_id = "other", .worker_id = worker_id},
                   WorkerHelloMessage{
                       .client_id = client_id, .session_generation = 1U, .worker_id = worker_id},
                   WorkerHelloMessage{.client_id = client_id, .worker_id = "other"},
                   WorkerHelloMessage{
                       .client_id = client_id, .worker_id = worker_id, .timestamp_seconds = 1},
                   [&] {
                       WorkerHelloMessage value{.client_id = client_id, .worker_id = worker_id};
                       value.nonce[0] = 1U;
                       return value;
                   }(),
                   [&] {
                       WorkerHelloMessage value{.client_id = client_id, .worker_id = worker_id};
                       value.authentication_data[0] = 1U;
                       return value;
                   }()});
    }
    {
        WorkerAcceptedMessage base{.worker_id = worker_id};
        expect_per_field_inequalities(base, {WorkerAcceptedMessage{.worker_id = "other"}});
    }
    {
        StartRelayMessage base{.tunnel_id = tunnel_id, .connection_id = connection_id};
        expect_per_field_inequalities(
            base,
            {StartRelayMessage{.tunnel_id = "other", .connection_id = connection_id},
             StartRelayMessage{.tunnel_id = tunnel_id, .connection_id = "other"},
             StartRelayMessage{
                 .tunnel_id = tunnel_id, .connection_id = connection_id, .mode = TunnelMode::udp}});
    }
    {
        LocalConnectOkMessage base{.connection_id = connection_id};
        expect_per_field_inequalities(base, {LocalConnectOkMessage{.connection_id = "other"}});
    }
    {
        LocalConnectErrorMessage base{.connection_id = connection_id};
        expect_per_field_inequalities(
            base, {LocalConnectErrorMessage{.connection_id = "other"},
                   LocalConnectErrorMessage{.connection_id = connection_id,
                                            .code = common::ErrorCode::connection_failed}});
    }
}

} // namespace
} // namespace minitun::protocol
