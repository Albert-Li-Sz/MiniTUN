#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/state_machine.hpp>

namespace minitun::protocol {
namespace {

TEST(RemoteStateMachineTest, CompletesControlHandshakeForBothPeers) {
    StateMachine client{PeerRole::client, ConnectionKind::control};
    StateMachine server{PeerRole::server, ConnectionKind::control};

    ASSERT_TRUE(client.on_send(MessageType::hello));
    ASSERT_TRUE(server.on_receive(MessageType::hello));
    ASSERT_TRUE(server.on_send(MessageType::hello_ack));
    ASSERT_TRUE(client.on_receive(MessageType::hello_ack));
    ASSERT_TRUE(client.on_send(MessageType::auth));
    ASSERT_TRUE(server.on_receive(MessageType::auth));
    ASSERT_TRUE(server.on_send(MessageType::auth_ok));
    ASSERT_TRUE(client.on_receive(MessageType::auth_ok));

    EXPECT_EQ(client.state(), ConnectionState::authenticated);
    EXPECT_EQ(server.state(), ConnectionState::authenticated);
    EXPECT_TRUE(client.on_send(MessageType::register_tunnel));
    EXPECT_TRUE(server.on_receive(MessageType::register_tunnel));
    EXPECT_TRUE(server.on_send(MessageType::register_tunnel_ok));
    EXPECT_TRUE(client.on_receive(MessageType::register_tunnel_ok));
    EXPECT_TRUE(server.on_send(MessageType::request_workers));
    EXPECT_TRUE(client.on_receive(MessageType::request_workers));
}

TEST(RemoteStateMachineTest, RejectsWrongDirectionTypeAndRepeatedHandshake) {
    StateMachine client{PeerRole::client, ConnectionKind::control};
    const auto wrong_direction = client.on_receive(MessageType::hello_ack);
    ASSERT_FALSE(wrong_direction);
    EXPECT_EQ(wrong_direction.error().code(), common::ErrorCode::protocol_error);
    EXPECT_EQ(client.state(), ConnectionState::client_send_hello);

    ASSERT_TRUE(client.on_send(MessageType::hello));
    const auto worker_message = client.on_receive(MessageType::worker_accepted);
    ASSERT_FALSE(worker_message);
    EXPECT_EQ(worker_message.error().code(), common::ErrorCode::protocol_error);
    EXPECT_EQ(client.state(), ConnectionState::client_wait_hello_ack);
}

TEST(RemoteStateMachineTest, AuthenticationFailureMovesConnectionToClosing) {
    StateMachine client{PeerRole::client, ConnectionKind::control};
    ASSERT_TRUE(client.on_send(MessageType::hello));
    ASSERT_TRUE(client.on_receive(MessageType::hello_ack));
    ASSERT_TRUE(client.on_send(MessageType::auth));
    ASSERT_TRUE(client.on_receive(MessageType::auth_error));
    EXPECT_EQ(client.state(), ConnectionState::closing);
    EXPECT_FALSE(client.framed_mode());
    EXPECT_FALSE(client.on_send(MessageType::hello));
}

TEST(RemoteStateMachineTest, WorkerHandshakeSwitchesPermanentlyToRawRelay) {
    StateMachine client{PeerRole::client, ConnectionKind::worker};
    StateMachine server{PeerRole::server, ConnectionKind::worker};

    ASSERT_TRUE(client.on_send(MessageType::worker_hello));
    ASSERT_TRUE(server.on_receive(MessageType::worker_hello));
    ASSERT_TRUE(server.on_send(MessageType::worker_accepted));
    ASSERT_TRUE(client.on_receive(MessageType::worker_accepted));
    ASSERT_TRUE(server.on_send(MessageType::start_relay));
    ASSERT_TRUE(client.on_receive(MessageType::start_relay));
    ASSERT_TRUE(client.on_send(MessageType::local_connect_ok));
    ASSERT_TRUE(server.on_receive(MessageType::local_connect_ok));

    EXPECT_EQ(client.state(), ConnectionState::relay);
    EXPECT_EQ(server.state(), ConnectionState::relay);
    EXPECT_FALSE(client.framed_mode());
    EXPECT_FALSE(server.framed_mode());
    EXPECT_FALSE(client.on_send(MessageType::local_connect_ok));
    EXPECT_FALSE(server.on_receive(MessageType::ping));
}

TEST(RemoteStateMachineTest, LocalConnectErrorClosesOnlyThatWorker) {
    StateMachine worker{PeerRole::client, ConnectionKind::worker};
    ASSERT_TRUE(worker.on_send(MessageType::worker_hello));
    ASSERT_TRUE(worker.on_receive(MessageType::worker_accepted));
    ASSERT_TRUE(worker.on_receive(MessageType::start_relay));
    ASSERT_TRUE(worker.on_send(MessageType::local_connect_error));
    EXPECT_EQ(worker.state(), ConnectionState::closing);
}

TEST(RemoteStateMachineTest, GoawayIsAcceptedFromAnyFramedHandshakeState) {
    StateMachine server{PeerRole::server, ConnectionKind::control};
    EXPECT_TRUE(server.on_send(MessageType::goaway));
    EXPECT_EQ(server.state(), ConnectionState::closing);
    EXPECT_EQ(to_string(server.state()), "closing");
}

} // namespace
} // namespace minitun::protocol
