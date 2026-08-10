#include <array>
#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/protocol/state_machine.hpp>

namespace minitun::protocol {
namespace {

void authenticate_control(StateMachine& machine) {
    if (machine.role() == PeerRole::client) {
        ASSERT_TRUE(machine.on_send(MessageType::hello));
        ASSERT_TRUE(machine.on_receive(MessageType::hello_ack));
        ASSERT_TRUE(machine.on_send(MessageType::auth));
        ASSERT_TRUE(machine.on_receive(MessageType::auth_ok));
    } else {
        ASSERT_TRUE(machine.on_receive(MessageType::hello));
        ASSERT_TRUE(machine.on_send(MessageType::hello_ack));
        ASSERT_TRUE(machine.on_receive(MessageType::auth));
        ASSERT_TRUE(machine.on_send(MessageType::auth_ok));
    }
    ASSERT_EQ(machine.state(), ConnectionState::authenticated);
}

TEST(RemoteStateMachineTest, ReportsEveryStateAndInitialPeerProperty) {
    constexpr std::array expected{
        std::pair{ConnectionState::client_send_hello, "client_send_hello"},
        std::pair{ConnectionState::client_wait_hello_ack, "client_wait_hello_ack"},
        std::pair{ConnectionState::client_send_auth, "client_send_auth"},
        std::pair{ConnectionState::client_wait_auth_result, "client_wait_auth_result"},
        std::pair{ConnectionState::server_wait_hello, "server_wait_hello"},
        std::pair{ConnectionState::server_send_hello_ack, "server_send_hello_ack"},
        std::pair{ConnectionState::server_wait_auth, "server_wait_auth"},
        std::pair{ConnectionState::server_send_auth_result, "server_send_auth_result"},
        std::pair{ConnectionState::authenticated, "authenticated"},
        std::pair{ConnectionState::client_send_worker_hello, "client_send_worker_hello"},
        std::pair{ConnectionState::client_wait_worker_accepted, "client_wait_worker_accepted"},
        std::pair{ConnectionState::server_wait_worker_hello, "server_wait_worker_hello"},
        std::pair{ConnectionState::server_send_worker_accepted, "server_send_worker_accepted"},
        std::pair{ConnectionState::worker_idle, "worker_idle"},
        std::pair{ConnectionState::client_connecting_local, "client_connecting_local"},
        std::pair{ConnectionState::server_wait_local_result, "server_wait_local_result"},
        std::pair{ConnectionState::relay, "relay"},
        std::pair{ConnectionState::closing, "closing"},
    };
    for (const auto& [state, name] : expected) {
        EXPECT_EQ(to_string(state), name);
    }
    EXPECT_EQ(to_string(static_cast<ConnectionState>(0xffU)), "unknown");

    StateMachine client_control{PeerRole::client, ConnectionKind::control};
    StateMachine server_control{PeerRole::server, ConnectionKind::control};
    StateMachine client_worker{PeerRole::client, ConnectionKind::worker};
    StateMachine server_worker{PeerRole::server, ConnectionKind::worker};
    EXPECT_EQ(client_control.role(), PeerRole::client);
    EXPECT_EQ(client_control.kind(), ConnectionKind::control);
    EXPECT_EQ(client_control.state(), ConnectionState::client_send_hello);
    EXPECT_TRUE(client_control.framed_mode());
    EXPECT_EQ(server_control.role(), PeerRole::server);
    EXPECT_EQ(server_control.kind(), ConnectionKind::control);
    EXPECT_EQ(server_control.state(), ConnectionState::server_wait_hello);
    EXPECT_EQ(client_worker.role(), PeerRole::client);
    EXPECT_EQ(client_worker.kind(), ConnectionKind::worker);
    EXPECT_EQ(client_worker.state(), ConnectionState::client_send_worker_hello);
    EXPECT_EQ(server_worker.role(), PeerRole::server);
    EXPECT_EQ(server_worker.kind(), ConnectionKind::worker);
    EXPECT_EQ(server_worker.state(), ConnectionState::server_wait_worker_hello);
}

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

TEST(RemoteStateMachineTest, RejectsWrongDirectionAtEveryControlHandshakeStep) {
    StateMachine client{PeerRole::client, ConnectionKind::control};
    EXPECT_FALSE(client.on_receive(MessageType::hello));
    ASSERT_TRUE(client.on_send(MessageType::hello));
    EXPECT_FALSE(client.on_send(MessageType::hello_ack));
    ASSERT_TRUE(client.on_receive(MessageType::hello_ack));
    EXPECT_FALSE(client.on_receive(MessageType::auth));
    ASSERT_TRUE(client.on_send(MessageType::auth));
    EXPECT_FALSE(client.on_send(MessageType::auth_ok));
    ASSERT_TRUE(client.on_receive(MessageType::auth_ok));

    StateMachine server{PeerRole::server, ConnectionKind::control};
    EXPECT_FALSE(server.on_send(MessageType::hello));
    ASSERT_TRUE(server.on_receive(MessageType::hello));
    EXPECT_FALSE(server.on_receive(MessageType::hello_ack));
    ASSERT_TRUE(server.on_send(MessageType::hello_ack));
    EXPECT_FALSE(server.on_send(MessageType::auth));
    ASSERT_TRUE(server.on_receive(MessageType::auth));
    EXPECT_FALSE(server.on_receive(MessageType::auth_ok));
    ASSERT_TRUE(server.on_send(MessageType::auth_ok));
}

TEST(RemoteStateMachineTest, AcceptsExactlyTheAuthenticatedControlMessageMatrix) {
    StateMachine client{PeerRole::client, ConnectionKind::control};
    StateMachine server{PeerRole::server, ConnectionKind::control};
    authenticate_control(client);
    authenticate_control(server);

    for (const auto type : {MessageType::register_tunnel, MessageType::unregister_tunnel,
                            MessageType::ping, MessageType::pong}) {
        EXPECT_TRUE(client.on_send(type)) << to_string(type);
        EXPECT_TRUE(server.on_receive(type)) << to_string(type);
    }
    for (const auto type : {MessageType::register_tunnel_ok, MessageType::register_tunnel_error,
                            MessageType::unregister_tunnel_ok, MessageType::request_workers,
                            MessageType::ping, MessageType::pong}) {
        EXPECT_TRUE(server.on_send(type)) << to_string(type);
        EXPECT_TRUE(client.on_receive(type)) << to_string(type);
    }

    EXPECT_FALSE(client.on_receive(MessageType::register_tunnel));
    EXPECT_FALSE(client.on_send(MessageType::register_tunnel_ok));
    EXPECT_FALSE(server.on_send(MessageType::register_tunnel));
    EXPECT_FALSE(server.on_receive(MessageType::register_tunnel_ok));
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

TEST(RemoteStateMachineTest, ServerAuthenticationFailureMovesConnectionToClosing) {
    StateMachine server{PeerRole::server, ConnectionKind::control};
    ASSERT_TRUE(server.on_receive(MessageType::hello));
    ASSERT_TRUE(server.on_send(MessageType::hello_ack));
    ASSERT_TRUE(server.on_receive(MessageType::auth));
    ASSERT_TRUE(server.on_send(MessageType::auth_error));
    EXPECT_EQ(server.state(), ConnectionState::closing);
    EXPECT_FALSE(server.on_receive(MessageType::hello));
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

TEST(RemoteStateMachineTest, ServerObservesLocalConnectErrorAndRejectsControlMessages) {
    StateMachine worker{PeerRole::server, ConnectionKind::worker};
    EXPECT_FALSE(worker.on_receive(MessageType::hello));
    EXPECT_FALSE(worker.on_send(MessageType::worker_accepted));
    ASSERT_TRUE(worker.on_receive(MessageType::worker_hello));
    EXPECT_FALSE(worker.on_receive(MessageType::worker_accepted));
    ASSERT_TRUE(worker.on_send(MessageType::worker_accepted));
    EXPECT_FALSE(worker.on_receive(MessageType::start_relay));
    ASSERT_TRUE(worker.on_send(MessageType::start_relay));
    EXPECT_FALSE(worker.on_send(MessageType::local_connect_error));
    ASSERT_TRUE(worker.on_receive(MessageType::local_connect_error));
    EXPECT_EQ(worker.state(), ConnectionState::closing);
}

TEST(RemoteStateMachineTest, ClientWorkerRejectsWrongDirectionsAtEachStep) {
    StateMachine worker{PeerRole::client, ConnectionKind::worker};
    EXPECT_FALSE(worker.on_receive(MessageType::worker_hello));
    ASSERT_TRUE(worker.on_send(MessageType::worker_hello));
    EXPECT_FALSE(worker.on_send(MessageType::worker_accepted));
    ASSERT_TRUE(worker.on_receive(MessageType::worker_accepted));
    EXPECT_FALSE(worker.on_send(MessageType::start_relay));
    ASSERT_TRUE(worker.on_receive(MessageType::start_relay));
    EXPECT_FALSE(worker.on_receive(MessageType::local_connect_ok));
    ASSERT_TRUE(worker.on_send(MessageType::local_connect_ok));
}

TEST(RemoteStateMachineTest, GoawayIsAcceptedFromAnyFramedHandshakeState) {
    StateMachine server{PeerRole::server, ConnectionKind::control};
    EXPECT_TRUE(server.on_send(MessageType::goaway));
    EXPECT_EQ(server.state(), ConnectionState::closing);
    EXPECT_EQ(to_string(server.state()), "closing");
}

TEST(RemoteStateMachineTest, ProtocolErrorMessageClosesWorkerFromFramedState) {
    StateMachine worker{PeerRole::client, ConnectionKind::worker};
    EXPECT_TRUE(worker.on_receive(MessageType::error));
    EXPECT_EQ(worker.state(), ConnectionState::closing);
    EXPECT_FALSE(worker.on_receive(MessageType::error));
}

} // namespace
} // namespace minitun::protocol
