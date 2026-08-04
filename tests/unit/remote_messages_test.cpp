#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/protocol/messages.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::string generated_id(const common::IdKind kind) {
    auto id = common::Id::generate(kind);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

TEST(RemoteMessagesTest, RoundTripsHandshakeAndHeartbeatPayloads) {
    AuthenticationNonce nonce{};
    AuthenticationData digest{};
    for (std::size_t index = 0U; index < nonce.size(); ++index) {
        nonce[index] = static_cast<std::uint8_t>(index);
        digest[index] = static_cast<std::uint8_t>(0xffU - index);
    }

    const HelloMessage hello{generated_id(common::IdKind::client)};
    const HelloAckMessage ack{generated_id(common::IdKind::server), -123, nonce};
    const AuthMessage auth{hello.client_id, 456, nonce, digest};
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
    ASSERT_GT(ack_payload->size(), kAuthenticationNonceSize);
    (*ack_payload)[ack_payload->size() - kAuthenticationNonceSize - 2U] = 0U;
    (*ack_payload)[ack_payload->size() - kAuthenticationNonceSize - 1U] = 31U;
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
    const RegisterTunnelMessage registration{tunnel_id, "0.0.0.0", 6'000U};
    const RegisterTunnelOkMessage registered{tunnel_id};
    const RegisterTunnelErrorMessage rejected{tunnel_id, common::ErrorCode::remote_port_in_use};
    const UnregisterTunnelMessage removal{tunnel_id};

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

TEST(RemoteMessagesTest, RejectsInvalidTunnelRegistrationPayloads) {
    const std::string tunnel_id = generated_id(common::IdKind::tunnel);
    EXPECT_FALSE(encode_register_tunnel({"tun_invalid", "0.0.0.0", 6'000U}));
    EXPECT_FALSE(encode_register_tunnel({tunnel_id, "", 6'000U}));
    EXPECT_FALSE(encode_register_tunnel({tunnel_id, "0.0.0.0", 0U}));
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
    const WorkerHelloMessage hello{client_id, 42U, worker_id};
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

} // namespace
} // namespace minitun::protocol
