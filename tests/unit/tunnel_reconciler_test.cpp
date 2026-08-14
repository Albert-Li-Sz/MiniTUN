#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <gtest/gtest.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/id.hpp>
#include <minitun/daemon/tunnel_reconciler.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::daemon {
namespace {

[[nodiscard]] common::Id require_id(const std::string_view value, const common::IdKind kind) {
    auto parsed = common::Id::parse(value, kind);
    if (!parsed) {
        throw std::runtime_error("invalid deterministic test ID");
    }
    return std::move(*parsed);
}

[[nodiscard]] common::Endpoint require_endpoint(const std::string_view value) {
    auto parsed = common::Endpoint::parse(value);
    if (!parsed) {
        throw std::runtime_error("invalid deterministic test endpoint");
    }
    return std::move(*parsed);
}

TEST(TunnelReconcilerTest, RejectsOldGenerationsAndStaleConfigurationRevisions) {
    storage::test::TemporaryDatabaseFile temporary;
    auto repository = storage::StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const auto server_id =
        require_id("srv_00000000000000000000000000000001", common::IdKind::server);
    const auto tunnel_id =
        require_id("tun_00000000000000000000000000000001", common::IdKind::tunnel);
    storage::ServerRecord server{
        .id = server_id,
        .name = "primary",
        .endpoint = require_endpoint("example.test:2333"),
        .credential_ref = "server/primary",
        .desired_state = storage::ServerDesiredState::enabled,
        .actual_state = storage::ServerActualState::online,
        .reconnect_attempt = 0,
        .created_at_unix_ms = 1,
        .updated_at_unix_ms = 1,
    };
    storage::TunnelRecord tunnel{
        .id = tunnel_id,
        .name = "web",
        .server_id = server_id,
        .protocol = storage::TunnelProtocol::tcp,
        .local_endpoint = require_endpoint("127.0.0.1:8080"),
        .remote_endpoint = require_endpoint("0.0.0.0:6000"),
        .desired_state = storage::TunnelDesiredState::active,
        .actual_state = storage::TunnelActualState::active,
        .created_at_unix_ms = 1,
        .updated_at_unix_ms = 1,
    };
    ASSERT_TRUE((*repository)->servers().create(server));
    ASSERT_TRUE((*repository)->tunnels().create(tunnel));

    TunnelReconciler reconciler{**repository};
    auto first_generation = reconciler.begin_generation(server_id);
    ASSERT_TRUE(first_generation) << first_generation.error();
    EXPECT_EQ((*repository)->tunnels().get_by_id(tunnel_id)->actual_state,
              storage::TunnelActualState::pending);

    auto registering = reconciler.transition(server_id, tunnel_id, *first_generation, 1,
                                             storage::TunnelActualState::registering);
    ASSERT_TRUE(registering) << registering.error();
    EXPECT_TRUE(*registering);

    auto second_generation = reconciler.begin_generation(server_id);
    ASSERT_TRUE(second_generation) << second_generation.error();
    ASSERT_NE(*first_generation, *second_generation);
    auto stale_generation =
        reconciler.transition(server_id, tunnel_id, *first_generation, 1,
                              storage::TunnelActualState::active, std::nullopt, true);
    ASSERT_TRUE(stale_generation) << stale_generation.error();
    EXPECT_FALSE(*stale_generation);

    auto revised = (*repository)->tunnels().get_by_id(tunnel_id);
    ASSERT_TRUE(revised) << revised.error();
    revised->config_revision = 2;
    revised->remote_endpoint = require_endpoint("0.0.0.0:6001");
    revised->actual_state = storage::TunnelActualState::pending;
    ++revised->updated_at_unix_ms;
    ASSERT_TRUE((*repository)->tunnels().update(*revised));

    auto stale_revision =
        reconciler.transition(server_id, tunnel_id, *second_generation, 1,
                              storage::TunnelActualState::active, std::nullopt, true);
    ASSERT_TRUE(stale_revision) << stale_revision.error();
    EXPECT_FALSE(*stale_revision);
    auto current_revision =
        reconciler.transition(server_id, tunnel_id, *second_generation, 2,
                              storage::TunnelActualState::active, std::nullopt, true);
    ASSERT_TRUE(current_revision) << current_revision.error();
    EXPECT_TRUE(*current_revision);

    ASSERT_TRUE(reconciler.invalidate(server_id));
    EXPECT_EQ((*repository)->tunnels().get_by_id(tunnel_id)->actual_state,
              storage::TunnelActualState::pending);
    auto after_invalidation = reconciler.transition(server_id, tunnel_id, *second_generation, 2,
                                                    storage::TunnelActualState::active);
    ASSERT_TRUE(after_invalidation) << after_invalidation.error();
    EXPECT_FALSE(*after_invalidation);

    EXPECT_FALSE(reconciler.is_current(tunnel_id, *second_generation));
    EXPECT_FALSE(reconciler.is_current(server_id, 0U));
    EXPECT_FALSE(reconciler.is_current(server_id, *second_generation));
    const auto current_generation = reconciler.begin_generation(server_id);
    ASSERT_TRUE(current_generation) << current_generation.error();
    EXPECT_TRUE(reconciler.is_current(server_id, *current_generation));
    EXPECT_TRUE(reconciler.end_generation(server_id, *second_generation));
    EXPECT_TRUE(reconciler.end_generation(server_id, *current_generation));

    const auto wrong_begin = reconciler.begin_generation(tunnel_id);
    ASSERT_FALSE(wrong_begin);
    EXPECT_EQ(wrong_begin.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_FALSE(reconciler.end_generation(tunnel_id, *current_generation));
    EXPECT_FALSE(reconciler.end_generation(server_id, 0U));
    EXPECT_FALSE(reconciler.invalidate(tunnel_id));
    EXPECT_FALSE(
        reconciler.transition(tunnel_id, tunnel_id, 1U, 1U, storage::TunnelActualState::active));
    EXPECT_FALSE(
        reconciler.transition(server_id, server_id, 1U, 1U, storage::TunnelActualState::active));
    EXPECT_FALSE(
        reconciler.transition(server_id, tunnel_id, 0U, 1U, storage::TunnelActualState::active));
    EXPECT_FALSE(reconciler.transition(server_id, tunnel_id, *current_generation, 0U,
                                       storage::TunnelActualState::active));

    const auto missing_tunnel =
        require_id("tun_00000000000000000000000000000002", common::IdKind::tunnel);
    const auto missing_transition = reconciler.transition(
        server_id, missing_tunnel, *current_generation, 1U, storage::TunnelActualState::active);
    ASSERT_TRUE(missing_transition) << missing_transition.error();
    EXPECT_FALSE(*missing_transition);

    auto active_again = (*repository)->tunnels().get_by_id(tunnel_id);
    ASSERT_TRUE(active_again) << active_again.error();
    active_again->actual_state = storage::TunnelActualState::active;
    ++active_again->updated_at_unix_ms;
    ASSERT_TRUE((*repository)->tunnels().update(*active_again));
    {
        storage::test::NativeSqliteDatabase native{temporary.path()};
        native.execute("CREATE TRIGGER reject_reconciliation BEFORE UPDATE OF actual_state ON "
                       "tunnels BEGIN SELECT RAISE(ABORT, 'reject reconciliation'); END");
    }
    const auto failed_end = reconciler.end_generation(
        server_id, *current_generation,
        common::Error{common::ErrorCode::connection_failed, "session ended"});
    ASSERT_FALSE(failed_end);
    EXPECT_EQ(failed_end.error().code(), common::ErrorCode::invalid_argument);
    const auto failed_invalidate = reconciler.invalidate(server_id);
    ASSERT_FALSE(failed_invalidate);
    EXPECT_EQ(failed_invalidate.error().code(), common::ErrorCode::invalid_argument);
    const auto failed_begin = reconciler.begin_generation(server_id);
    ASSERT_FALSE(failed_begin);
    EXPECT_EQ(failed_begin.error().code(), common::ErrorCode::invalid_argument);
}

} // namespace
} // namespace minitun::daemon
