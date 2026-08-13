#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/remote_protocol.hpp>

namespace minitun::remote {
namespace {

TEST(RemoteProtocolSdkTest, RoundTripsTypedFramesIncrementally) {
    const protocol::RegisterTunnelMessage registration{
        "tun_00000000000000000000000000000001", "127.0.0.1", 6500U, 9U, protocol::TunnelMode::udp};
    const Message message{registration};
    auto frame = Codec::make_frame(protocol::MessageType::register_tunnel, 42U, message);
    ASSERT_TRUE(frame) << frame.error();
    auto bytes = Codec::encode_frame(*frame);
    ASSERT_TRUE(bytes) << bytes.error();

    Decoder decoder;
    std::vector<protocol::Frame> decoded;
    for (const std::uint8_t byte : *bytes) {
        const std::array one{byte};
        auto fragment = decoder.feed(one);
        ASSERT_TRUE(fragment) << fragment.error();
        decoded.insert(decoded.end(), fragment->begin(), fragment->end());
    }
    ASSERT_TRUE(decoder.finish());
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded[0], *frame);
    auto typed = Codec::decode_message(decoded[0]);
    ASSERT_TRUE(typed) << typed.error();
    ASSERT_TRUE(std::holds_alternative<protocol::RegisterTunnelMessage>(*typed));
    EXPECT_EQ(std::get<protocol::RegisterTunnelMessage>(*typed), registration);
}

TEST(RemoteProtocolSdkTest, RejectsMismatchedTypedMessagesAndPartialFrames) {
    const Message heartbeat{protocol::HeartbeatMessage{7U}};
    const auto mismatch = Codec::make_frame(protocol::MessageType::hello, 1U, heartbeat);
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code(), common::ErrorCode::invalid_argument);

    Decoder decoder;
    const std::array<std::uint8_t, 3U> partial{0x4dU, 0x54U, 0x55U};
    ASSERT_TRUE(decoder.feed(partial));
    const auto finished = decoder.finish();
    ASSERT_FALSE(finished);
    EXPECT_EQ(finished.error().code(), common::ErrorCode::protocol_error);
    decoder.reset();
    EXPECT_TRUE(decoder.finish());
}

TEST(RemoteProtocolSdkTest, MovedFromDecoderRemainsSafelyDestructible) {
    Decoder source;
    Decoder destination{std::move(source)};
    EXPECT_TRUE(destination.finish());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    const auto feed = source.feed({});
    ASSERT_FALSE(feed);
    EXPECT_EQ(feed.error().code(), common::ErrorCode::invalid_argument);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    const auto finish = source.finish();
    ASSERT_FALSE(finish);
    EXPECT_EQ(finish.error().code(), common::ErrorCode::invalid_argument);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    source.reset();
}

TEST(RemoteProtocolSdkTest, MoveAssignmentLeavesSourceMovedFrom) {
    Decoder source;
    Decoder destination;
    destination = std::move(source);
    EXPECT_TRUE(destination.finish());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    const auto feed = source.feed({});
    ASSERT_FALSE(feed);
    EXPECT_EQ(feed.error().code(), common::ErrorCode::invalid_argument);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    source.reset();
}

TEST(RemoteProtocolSdkTest, RoundTripsEveryMessageType) {
    const std::string client_id{"client_00000000000000000000000000000001"};
    const std::string server_id{"srv_00000000000000000000000000000001"};
    const std::string tunnel_id{"tun_00000000000000000000000000000001"};
    const std::string connection_id{"conn_00000000000000000000000000000001"};
    const protocol::AuthenticationNonce nonce{};
    const protocol::AuthenticationData data{};

    const std::array<std::pair<protocol::MessageType, Message>, 19U> cases{{
        {protocol::MessageType::hello, Message{protocol::HelloMessage{client_id}}},
        {protocol::MessageType::hello_ack,
         Message{protocol::HelloAckMessage{server_id, 1'700'000'000, nonce}}},
        {protocol::MessageType::auth,
         Message{protocol::AuthMessage{client_id, 1'700'000'000, nonce, data}}},
        {protocol::MessageType::auth_ok, Message{protocol::AuthOkMessage{9U, 5'000U, 2U, 32U}}},
        {protocol::MessageType::auth_error,
         Message{protocol::AuthErrorMessage{common::ErrorCode::authentication_failed}}},
        {protocol::MessageType::ping, Message{protocol::HeartbeatMessage{7U}}},
        {protocol::MessageType::pong, Message{protocol::HeartbeatMessage{8U}}},
        {protocol::MessageType::register_tunnel,
         Message{protocol::RegisterTunnelMessage{tunnel_id, "127.0.0.1", 6500U, 9U,
                                                 protocol::TunnelMode::udp}}},
        {protocol::MessageType::register_tunnel_ok,
         Message{protocol::RegisterTunnelOkMessage{tunnel_id, 9U}}},
        {protocol::MessageType::register_tunnel_error,
         Message{protocol::RegisterTunnelErrorMessage{
             tunnel_id, common::ErrorCode::remote_port_in_use, 9U}}},
        {protocol::MessageType::unregister_tunnel,
         Message{protocol::UnregisterTunnelMessage{tunnel_id, 9U}}},
        {protocol::MessageType::unregister_tunnel_ok,
         Message{protocol::UnregisterTunnelMessage{tunnel_id, 9U}}},
        {protocol::MessageType::request_workers, Message{protocol::RequestWorkersMessage{4U}}},
        {protocol::MessageType::worker_hello,
         Message{protocol::WorkerHelloMessage{client_id, 3U, connection_id, 1'700'000'000,
                                              nonce, data}}},
        {protocol::MessageType::worker_accepted,
         Message{protocol::WorkerAcceptedMessage{connection_id}}},
        {protocol::MessageType::start_relay,
         Message{protocol::StartRelayMessage{tunnel_id, connection_id,
                                             protocol::TunnelMode::socks5}}},
        {protocol::MessageType::local_connect_ok,
         Message{protocol::LocalConnectOkMessage{connection_id}}},
        {protocol::MessageType::local_connect_error,
         Message{protocol::LocalConnectErrorMessage{
             connection_id, common::ErrorCode::local_connect_failed}}},
        {protocol::MessageType::goaway, Message{EmptyMessage{protocol::MessageType::goaway}}},
    }};

    for (const auto& [type, message] : cases) {
        auto frame = Codec::make_frame(type, 1U, message);
        ASSERT_TRUE(frame) << frame.error();
        auto decoded = Codec::decode_message(*frame);
        ASSERT_TRUE(decoded) << decoded.error();
        EXPECT_EQ(*decoded, message);
    }

    const auto error_frame = Codec::make_frame(
        protocol::MessageType::error, 2U,
        Message{EmptyMessage{protocol::MessageType::error}});
    ASSERT_TRUE(error_frame) << error_frame.error();
    const auto error_decoded = Codec::decode_message(*error_frame);
    ASSERT_TRUE(error_decoded) << error_decoded.error();
    EXPECT_EQ(*error_decoded, Message{EmptyMessage{protocol::MessageType::error}});
}

TEST(RemoteProtocolSdkTest, RejectsUnsupportedTypesAndMalformedEmptyMessages) {
    const auto mismatch = Codec::make_frame(
        protocol::MessageType::goaway, 1U,
        Message{EmptyMessage{protocol::MessageType::error}});
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code(), common::ErrorCode::invalid_argument);

    const auto unknown = Codec::make_frame(
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        static_cast<protocol::MessageType>(65535U), 1U,
        Message{protocol::HeartbeatMessage{1U}});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code(), common::ErrorCode::invalid_argument);

    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const protocol::Frame unknown_frame{static_cast<protocol::MessageType>(65535U), 0U, 1U, {}};
    const auto unknown_decoded = Codec::decode_message(unknown_frame);
    ASSERT_FALSE(unknown_decoded);
    EXPECT_EQ(unknown_decoded.error().code(), common::ErrorCode::protocol_error);

    const protocol::Frame goaway_with_payload{protocol::MessageType::goaway, 0U, 1U, {0x01U}};
    const auto payload_rejected = Codec::decode_message(goaway_with_payload);
    ASSERT_FALSE(payload_rejected);
    EXPECT_EQ(payload_rejected.error().code(), common::ErrorCode::protocol_error);

    const protocol::Frame truncated{protocol::MessageType::hello, 0U, 1U, {0x00U}};
    const auto truncated_decoded = Codec::decode_message(truncated);
    ASSERT_FALSE(truncated_decoded);
}

TEST(RemoteProtocolSdkTest, ComputesAuthenticationDataThroughCodec) {
    const std::string client_id{"client_00000000000000000000000000000001"};
    const std::string server_id{"srv_00000000000000000000000000000001"};
    const std::string connection_id{"conn_00000000000000000000000000000001"};
    const protocol::AuthenticationNonce nonce{};

    const auto control = Codec::control_authentication_data(
        "team-a-psk", client_id, server_id, 1'700'000'000, nonce,
        protocol::kSupportedCapabilities);
    ASSERT_TRUE(control) << control.error();
    EXPECT_NE(*control, protocol::AuthenticationData{});

    const auto worker = Codec::worker_authentication_data(
        "team-a-psk", client_id, server_id, 3U, connection_id, 1'700'000'000, nonce);
    ASSERT_TRUE(worker) << worker.error();
    EXPECT_NE(*worker, protocol::AuthenticationData{});

    const auto invalid = Codec::control_authentication_data(
        "team-a-psk", client_id, server_id, 1'700'000'000, nonce, 0U);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), common::ErrorCode::invalid_argument);
}

} // namespace
} // namespace minitun::remote
