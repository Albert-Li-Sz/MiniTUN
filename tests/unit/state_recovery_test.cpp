#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::storage {
namespace {

[[nodiscard]] common::Id recovery_id(const common::IdKind kind, const std::uint64_t number) {
    std::ostringstream suffix;
    suffix << std::hex << std::nouppercase << std::setfill('0') << std::setw(32) << number;
    auto parsed = common::Id::parse(std::string{common::id_prefix(kind)} + suffix.str(), kind);
    if (!parsed) {
        throw std::runtime_error("failed to construct recovery-test ID");
    }
    return std::move(*parsed);
}

[[nodiscard]] common::Endpoint recovery_endpoint(const std::string_view value) {
    auto parsed = common::Endpoint::parse(value);
    if (!parsed) {
        throw std::runtime_error("failed to construct recovery-test endpoint");
    }
    return std::move(*parsed);
}

[[nodiscard]] ServerRecord recovery_server(const std::uint64_t number,
                                           const ServerDesiredState desired_state,
                                           const ServerActualState actual_state,
                                           const bool has_credential) {
    return ServerRecord{
        .id = recovery_id(common::IdKind::server, number),
        .name = "server-" + std::to_string(number),
        .endpoint = recovery_endpoint("server.example.com:2333"),
        .credential_ref = has_credential
                              ? std::optional<std::string>{"credential-" + std::to_string(number)}
                              : std::nullopt,
        .remote_server_id = "remote-" + std::to_string(number),
        .desired_state = desired_state,
        .actual_state = actual_state,
        .last_error_code = common::ErrorCode::connection_timeout,
        .last_error_message = "historical timeout",
        .reconnect_attempt = 7,
        .latency_ms = 88,
        .created_at_unix_ms = static_cast<std::int64_t>(10'000 + number),
        .updated_at_unix_ms = static_cast<std::int64_t>(20'000 + number),
    };
}

[[nodiscard]] TunnelRecord recovery_tunnel(const std::uint64_t number, const common::Id& server_id,
                                           const TunnelDesiredState desired_state,
                                           const TunnelActualState actual_state,
                                           const std::uint16_t remote_port) {
    return TunnelRecord{
        .id = recovery_id(common::IdKind::tunnel, number),
        .name = "tunnel-" + std::to_string(number),
        .server_id = server_id,
        .protocol = TunnelProtocol::tcp,
        .local_endpoint = recovery_endpoint("127.0.0.1:8080"),
        .remote_endpoint = recovery_endpoint("0.0.0.0:" + std::to_string(remote_port)),
        .desired_state = desired_state,
        .actual_state = actual_state,
        .last_error_code = common::ErrorCode::local_connect_failed,
        .last_error_message = "historical local failure",
        .created_at_unix_ms = static_cast<std::int64_t>(30'000 + number),
        .updated_at_unix_ms = static_cast<std::int64_t>(40'000 + number),
    };
}

TEST(StateRecoveryTest, NormalizesTransientStateAndIsIdempotentAcrossReopen) {
    test::TemporaryDatabaseFile temporary;
    RecoverySnapshot first_snapshot{
        .servers = {},
        .tunnels = {},
    };

    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();

        const ServerRecord unauthenticated =
            recovery_server(1, ServerDesiredState::enabled, ServerActualState::online, false);
        const ServerRecord reconnectable =
            recovery_server(2, ServerDesiredState::enabled, ServerActualState::connecting, true);
        const ServerRecord disabled =
            recovery_server(3, ServerDesiredState::disabled, ServerActualState::error, true);
        const ServerRecord removed =
            recovery_server(4, ServerDesiredState::removed, ServerActualState::online, true);

        ASSERT_TRUE((*repository)->servers().create(unauthenticated));
        ASSERT_TRUE((*repository)->servers().create(reconnectable));
        ASSERT_TRUE((*repository)->servers().create(disabled));
        ASSERT_TRUE((*repository)->servers().create(removed));

        ASSERT_TRUE((*repository)
                        ->tunnels()
                        .create(recovery_tunnel(1, unauthenticated.id, TunnelDesiredState::active,
                                                TunnelActualState::active, 6'000)));
        ASSERT_TRUE((*repository)
                        ->tunnels()
                        .create(recovery_tunnel(2, reconnectable.id, TunnelDesiredState::disabled,
                                                TunnelActualState::registering, 6'000)));
        ASSERT_TRUE((*repository)
                        ->tunnels()
                        .create(recovery_tunnel(3, reconnectable.id, TunnelDesiredState::removed,
                                                TunnelActualState::failed, 6'001)));
        ASSERT_TRUE((*repository)
                        ->tunnels()
                        .create(recovery_tunnel(4, removed.id, TunnelDesiredState::active,
                                                TunnelActualState::active, 6'000)));

        auto recovered = (*repository)->recover();
        ASSERT_TRUE(recovered) << recovered.error();
        first_snapshot = std::move(*recovered);
        ASSERT_EQ(first_snapshot.servers.size(), 4U);
        ASSERT_EQ(first_snapshot.tunnels.size(), 4U);

        const auto first_server = (*repository)->servers().get_by_id(unauthenticated.id);
        ASSERT_TRUE(first_server) << first_server.error();
        EXPECT_EQ(first_server->actual_state, ServerActualState::not_authenticated);
        EXPECT_EQ(first_server->reconnect_attempt, 0U);
        EXPECT_EQ(first_server->latency_ms, std::nullopt);
        EXPECT_EQ(first_server->remote_server_id, unauthenticated.remote_server_id);
        EXPECT_EQ(first_server->last_error_code, unauthenticated.last_error_code);

        const auto second_server = (*repository)->servers().get_by_id(reconnectable.id);
        ASSERT_TRUE(second_server) << second_server.error();
        EXPECT_EQ(second_server->actual_state, ServerActualState::disconnected);

        const auto disabled_server = (*repository)->servers().get_by_id(disabled.id);
        ASSERT_TRUE(disabled_server) << disabled_server.error();
        EXPECT_EQ(disabled_server->actual_state, ServerActualState::disabled);

        const auto removed_server = (*repository)->servers().get_by_id(removed.id);
        ASSERT_TRUE(removed_server) << removed_server.error();
        EXPECT_EQ(removed_server->actual_state, ServerActualState::disabled);

        const auto active_tunnel =
            (*repository)->tunnels().get_by_id(recovery_id(common::IdKind::tunnel, 1));
        ASSERT_TRUE(active_tunnel) << active_tunnel.error();
        EXPECT_EQ(active_tunnel->actual_state, TunnelActualState::pending);

        const auto disabled_tunnel =
            (*repository)->tunnels().get_by_id(recovery_id(common::IdKind::tunnel, 2));
        ASSERT_TRUE(disabled_tunnel) << disabled_tunnel.error();
        EXPECT_EQ(disabled_tunnel->actual_state, TunnelActualState::disabled);

        const auto removed_tunnel =
            (*repository)->tunnels().get_by_id(recovery_id(common::IdKind::tunnel, 3));
        ASSERT_TRUE(removed_tunnel) << removed_tunnel.error();
        EXPECT_EQ(removed_tunnel->actual_state, TunnelActualState::removing);

        const auto child_of_removed =
            (*repository)->tunnels().get_by_id(recovery_id(common::IdKind::tunnel, 4));
        ASSERT_TRUE(child_of_removed) << child_of_removed.error();
        EXPECT_EQ(child_of_removed->desired_state, TunnelDesiredState::removed);
        EXPECT_EQ(child_of_removed->actual_state, TunnelActualState::removing);

        const auto second_recovery = (*repository)->recover();
        ASSERT_TRUE(second_recovery) << second_recovery.error();
        EXPECT_EQ(*second_recovery, first_snapshot);
    }

    auto reopened = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(reopened) << reopened.error();
    const auto after_restart = (*reopened)->recover();
    ASSERT_TRUE(after_restart) << after_restart.error();
    EXPECT_EQ(*after_restart, first_snapshot);
}

TEST(StateRecoveryTest, InvalidPersistedStateRollsBackTheWholeRecovery) {
    test::TemporaryDatabaseFile temporary;
    const ServerRecord server =
        recovery_server(10, ServerDesiredState::enabled, ServerActualState::online, true);

    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();
    ASSERT_TRUE((*repository)->servers().create(server));
    {
        test::NativeSqliteDatabase native{temporary.path()};
        native.execute("PRAGMA ignore_check_constraints = ON");
        native.execute("UPDATE servers SET desired_state = 'invalid-state', "
                       "actual_state = 'online', reconnect_attempt = 7, latency_ms = 88");
    }
    const auto recovered = (*repository)->recover();
    ASSERT_FALSE(recovered);
    EXPECT_EQ(recovered.error().code(), common::ErrorCode::database_error);

    {
        test::NativeSqliteDatabase native{temporary.path()};
        EXPECT_EQ(native.query_text("SELECT actual_state FROM servers"), "online");
        EXPECT_EQ(native.query_int64("SELECT reconnect_attempt FROM servers"), 7);
        EXPECT_EQ(native.query_int64("SELECT latency_ms FROM servers"), 88);
    }
}

} // namespace
} // namespace minitun::storage
