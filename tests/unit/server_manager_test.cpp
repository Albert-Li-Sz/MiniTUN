#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/daemon/server_manager.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::daemon {
namespace {

[[nodiscard]] common::Id generated_id(const common::IdKind kind) {
    auto id = common::Id::generate(kind);
    if (!id) {
        throw std::runtime_error("failed to generate server-manager test ID");
    }
    return std::move(*id);
}

[[nodiscard]] common::Endpoint endpoint(const std::string& value) {
    auto parsed = common::Endpoint::parse(value);
    if (!parsed) {
        throw std::runtime_error("invalid server-manager test endpoint");
    }
    return std::move(*parsed);
}

[[nodiscard]] storage::ServerRecord
server_record(const storage::ServerDesiredState desired_state,
              std::optional<std::string> credential_ref = std::nullopt) {
    return {
        .id = generated_id(common::IdKind::server),
        .name = std::string{"managed"},
        .endpoint = endpoint("127.0.0.1:9"),
        .credential_ref = std::move(credential_ref),
        .desired_state = desired_state,
        .actual_state = desired_state == storage::ServerDesiredState::removed
                            ? storage::ServerActualState::disabled
                            : storage::ServerActualState::disconnected,
        .reconnect_attempt = 0U,
        .created_at_unix_ms = 1,
        .updated_at_unix_ms = 1,
    };
}

[[nodiscard]] storage::TunnelRecord
removed_tunnel_record(const storage::ServerRecord& server) {
    return {
        .id = generated_id(common::IdKind::tunnel),
        .name = "tombstone",
        .server_id = server.id,
        .protocol = storage::TunnelProtocol::tcp,
        .local_endpoint = endpoint("127.0.0.1:8080"),
        .remote_endpoint = endpoint("0.0.0.0:7000"),
        .desired_state = storage::TunnelDesiredState::removed,
        .actual_state = storage::TunnelActualState::removing,
        .created_at_unix_ms = 1,
        .updated_at_unix_ms = 1,
    };
}

[[nodiscard]] std::int64_t future_updated_at() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return now + 60'000;
}

struct ManagerFixture final {
    storage::test::TemporaryDatabaseFile state_file;
    storage::test::TemporaryDatabaseFile credential_file;
    std::unique_ptr<storage::StateRepository> repository;
    std::unique_ptr<storage::SqliteCredentialStore> credentials;

    ManagerFixture() {
        auto opened_repository = storage::StateRepository::open(state_file.path_string());
        if (!opened_repository) {
            throw std::runtime_error("failed to open manager state fixture");
        }
        repository = std::move(*opened_repository);
        auto opened_credentials =
            storage::SqliteCredentialStore::open(credential_file.path_string());
        if (!opened_credentials) {
            throw std::runtime_error("failed to open manager credential fixture");
        }
        credentials = std::move(*opened_credentials);
    }
};

TEST(ServerManagerTest, RejectsEveryIndependentOptionBoundary) {
    ManagerFixture fixture;
    asio::io_context io_context;
    const common::Id client_id = generated_id(common::IdKind::client);
    const auto expect_invalid = [&](ServerManagerOptions options) {
        const auto result = ServerManager::create(
            io_context, *fixture.repository, *fixture.credentials, client_id, std::move(options));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument) << result.error();
    };

    const auto expect_bad_duration = [&expect_invalid](auto member, const auto value) {
        ServerManagerOptions options;
        options.*member = value;
        expect_invalid(std::move(options));
    };
    expect_bad_duration(&ServerManagerOptions::reconcile_interval, std::chrono::milliseconds{49});
    expect_bad_duration(&ServerManagerOptions::reconcile_interval,
                        std::chrono::seconds{10} + std::chrono::milliseconds{1});
    expect_bad_duration(&ServerManagerOptions::connect_timeout, std::chrono::seconds::zero());
    expect_bad_duration(&ServerManagerOptions::connect_timeout, std::chrono::seconds{301});
    expect_bad_duration(&ServerManagerOptions::handshake_timeout, std::chrono::seconds::zero());
    expect_bad_duration(&ServerManagerOptions::handshake_timeout, std::chrono::seconds{301});
    expect_bad_duration(&ServerManagerOptions::relay_inactivity_timeout,
                        std::chrono::seconds::zero());
    expect_bad_duration(&ServerManagerOptions::relay_inactivity_timeout,
                        std::chrono::hours{24} + std::chrono::seconds{1});
    expect_bad_duration(&ServerManagerOptions::graceful_shutdown_timeout,
                        std::chrono::seconds::zero());
    expect_bad_duration(&ServerManagerOptions::graceful_shutdown_timeout,
                        std::chrono::seconds{301});

    const auto expect_bad_limit = [&expect_invalid](auto member, const std::size_t value) {
        ServerManagerOptions options;
        options.*member = value;
        expect_invalid(std::move(options));
    };
    expect_bad_limit(&ServerManagerOptions::max_idle_workers_per_server, 0U);
    expect_bad_limit(&ServerManagerOptions::max_idle_workers_per_server, 129U);
    expect_bad_limit(&ServerManagerOptions::max_total_idle_workers, 0U);
    expect_bad_limit(&ServerManagerOptions::max_total_idle_workers, 4'097U);
    expect_bad_limit(&ServerManagerOptions::max_total_connections, 0U);
    expect_bad_limit(&ServerManagerOptions::max_total_connections, 100'001U);
    ServerManagerOptions inverted;
    inverted.max_total_idle_workers = 100U;
    inverted.max_total_connections = 99U;
    expect_invalid(std::move(inverted));
}

TEST(ServerManagerTest, RejectsWrongIdentityAndUnavailableGlobalCa) {
    ManagerFixture fixture;
    asio::io_context io_context;
    const auto wrong_id =
        ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                              generated_id(common::IdKind::server));
    ASSERT_FALSE(wrong_id);
    EXPECT_EQ(wrong_id.error().code(), common::ErrorCode::invalid_argument);

    ServerManagerOptions options;
    options.ca_certificate_path = "/does/not/exist/ca.pem";
    const auto missing_ca =
        ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                              generated_id(common::IdKind::client), std::move(options));
    ASSERT_FALSE(missing_ca);
    EXPECT_EQ(missing_ca.error().code(), common::ErrorCode::tls_error);
}

TEST(ServerManagerTest, EmptyLifecycleAndMetricsAreStableAndIdempotent) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    EXPECT_EQ((*manager)->session_count(), 0U);
    const auto metrics = (*manager)->metrics();
    EXPECT_EQ(metrics.at("sessions").at("active"), 0U);
    EXPECT_EQ(metrics.at("workers").at("idle"), 0U);
    EXPECT_EQ(metrics.at("workers").at("active"), 0U);
    EXPECT_EQ(metrics.at("errors"), 0U);
    EXPECT_EQ(metrics.at("p2p_paths").at("direct"), 0U);
    EXPECT_EQ(metrics.at("p2p_paths").at("relay"), 0U);
    EXPECT_EQ(metrics.at("p2p_udp").at("datagrams_in"), 0U);
    EXPECT_EQ(metrics.at("p2p_udp").at("datagrams_out"), 0U);
    EXPECT_EQ(metrics.at("p2p_udp").at("bytes_in"), 0U);
    EXPECT_EQ(metrics.at("p2p_udp").at("bytes_out"), 0U);

    (*manager)->notify_changed();
    (*manager)->reload();
    (*manager)->stop();
    ASSERT_TRUE((*manager)->start());
    const auto duplicate = (*manager)->start();
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);
    (*manager)->notify_changed();
    (*manager)->reload();
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 0U);
    (*manager)->stop();
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, PurgesRemovedResourceAndAllCredentialReferences) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto removed = server_record(storage::ServerDesiredState::removed, "server/psk");
    removed.ca_credential_ref = "server/ca";
    removed.client_certificate_ref = "server/certificate";
    removed.client_private_key_ref = "server/private-key";
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.credentials->put("server/ca", "ca"));
    ASSERT_TRUE(fixture.credentials->put("server/certificate", "certificate"));
    ASSERT_TRUE(fixture.credentials->put("server/private-key", "private-key"));
    ASSERT_TRUE(fixture.repository->servers().create(removed));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ(fixture.repository->servers().get_by_id(removed.id).error().code(),
              common::ErrorCode::not_found);
    for (const auto key : {"server/psk", "server/ca", "server/certificate", "server/private-key"}) {
        EXPECT_EQ(fixture.credentials->get(key).error().code(), common::ErrorCode::not_found);
    }
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, LeavesDisabledAndUnauthenticatedRecordsWithoutSessions) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto disabled = server_record(storage::ServerDesiredState::disabled, "server/disabled");
    auto unauthenticated = server_record(storage::ServerDesiredState::enabled);
    unauthenticated.name = "unauthenticated";
    ASSERT_TRUE(fixture.repository->servers().create(disabled));
    ASSERT_TRUE(fixture.repository->servers().create(unauthenticated));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ((*manager)->session_count(), 0U);
    EXPECT_TRUE(fixture.repository->servers().get_by_id(disabled.id));
    EXPECT_TRUE(fixture.repository->servers().get_by_id(unauthenticated.id));
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, NotifyBeforeStartNeverCreatesSessions) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    (*manager)->notify_changed();
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 0U);

    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ((*manager)->session_count(), 1U);
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, StartsSessionForEnabledServerAndStopsItWhenDisabled) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ((*manager)->session_count(), 1U);

    auto current = fixture.repository->servers().get_by_id(enabled.id);
    ASSERT_TRUE(current) << current.error();
    current->desired_state = storage::ServerDesiredState::disabled;
    current->actual_state = storage::ServerActualState::disabled;
    current->config_revision += 1U;
    current->updated_at_unix_ms = future_updated_at();
    ASSERT_TRUE(fixture.repository->servers().update(*current));

    (*manager)->notify_changed();
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 0U);
    EXPECT_TRUE(fixture.repository->servers().get_by_id(enabled.id));
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, RestartsSessionWhenEndpointOrTlsMaterialChanges) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ((*manager)->session_count(), 1U);

    auto endpoint_change = fixture.repository->servers().get_by_id(enabled.id);
    ASSERT_TRUE(endpoint_change) << endpoint_change.error();
    endpoint_change->endpoint = endpoint("127.0.0.1:10");
    endpoint_change->config_revision += 1U;
    endpoint_change->updated_at_unix_ms = future_updated_at();
    ASSERT_TRUE(fixture.repository->servers().update(*endpoint_change));

    (*manager)->notify_changed();
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 1U);

    auto ca_change = fixture.repository->servers().get_by_id(enabled.id);
    ASSERT_TRUE(ca_change) << ca_change.error();
    ca_change->ca_credential_ref = "server/missing-ca";
    ca_change->config_revision += 1U;
    ca_change->updated_at_unix_ms = future_updated_at() + 1;
    ASSERT_TRUE(fixture.repository->servers().update(*ca_change));

    (*manager)->notify_changed();
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 0U);
    EXPECT_TRUE(fixture.repository->servers().get_by_id(enabled.id));
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, RevisionOnlyChangeRefreshesSessionWithoutRestart) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ((*manager)->session_count(), 1U);

    auto revised = fixture.repository->servers().get_by_id(enabled.id);
    ASSERT_TRUE(revised) << revised.error();
    revised->config_revision += 1U;
    revised->updated_at_unix_ms = future_updated_at();
    ASSERT_TRUE(fixture.repository->servers().update(*revised));

    (*manager)->notify_changed();
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 1U);
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, ReloadRecreatesSessionWithRotatedCredentialSlot) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "old-psk"));
    ASSERT_TRUE(fixture.credentials->put("server/psk/next", "new-psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ((*manager)->session_count(), 1U);

    auto rotated = fixture.repository->servers().get_by_id(enabled.id);
    ASSERT_TRUE(rotated) << rotated.error();
    rotated->credential_ref = "server/psk/next";
    rotated->config_revision += 1U;
    rotated->updated_at_unix_ms = future_updated_at();
    ASSERT_TRUE(fixture.repository->servers().update(*rotated));

    (*manager)->reload();
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 1U);
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, PurgesRemovedTunnelTombstonesForRetainedServer) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));
    auto tombstone = removed_tunnel_record(enabled);
    ASSERT_TRUE(fixture.repository->tunnels().create(tombstone));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ((*manager)->session_count(), 1U);
    EXPECT_EQ(fixture.repository->tunnels().get_by_id(tombstone.id).error().code(),
              common::ErrorCode::not_found);
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, GenerationFailureStopsSessionAndCountsPersistenceError) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));
    auto tunnel = removed_tunnel_record(enabled);
    tunnel.desired_state = storage::TunnelDesiredState::active;
    tunnel.actual_state = storage::TunnelActualState::active;
    ASSERT_TRUE(fixture.repository->tunnels().create(tunnel));

    {
        storage::test::NativeSqliteDatabase native{fixture.state_file.path()};
        native.execute("DROP TABLE tunnels");
        native.execute("CREATE TABLE tunnels(id TEXT PRIMARY KEY)");
    }

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    for (int attempt = 0; attempt < 50; ++attempt) {
        io_context.poll();
        if ((*manager)->metrics().at("persistence_errors").get<std::uint64_t>() != 0U) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    EXPECT_GE((*manager)->metrics().at("persistence_errors").get<std::uint64_t>(), 1U);
    (*manager)->stop();
    io_context.run();
}

TEST(ServerManagerTest, DisconnectPersistenceFailureIsCountedAndSessionStillStops) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));
    auto tunnel = removed_tunnel_record(enabled);
    tunnel.desired_state = storage::TunnelDesiredState::active;
    tunnel.actual_state = storage::TunnelActualState::pending;
    ASSERT_TRUE(fixture.repository->tunnels().create(tunnel));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    io_context.poll();
    {
        storage::test::NativeSqliteDatabase native{fixture.state_file.path()};
        native.execute("CREATE TRIGGER reject_tunnel_reconcile BEFORE UPDATE OF actual_state ON "
                       "tunnels BEGIN SELECT RAISE(ABORT, 'injected reconcile failure'); END");
    }

    (*manager)->stop();
    io_context.run();
    EXPECT_GE((*manager)->metrics().at("persistence_errors").get<std::uint64_t>(), 1U);
    EXPECT_EQ((*manager)->session_count(), 0U);
}

TEST(ServerManagerTest, EveryTlsMaterialChangeRestartsSessionAndMissingMaterialBlocksIt) {
    ManagerFixture fixture;
    asio::io_context io_context;
    auto enabled = server_record(storage::ServerDesiredState::enabled, "server/psk");
    ASSERT_TRUE(fixture.credentials->put("server/psk", "psk"));
    ASSERT_TRUE(fixture.repository->servers().create(enabled));

    auto manager = ServerManager::create(io_context, *fixture.repository, *fixture.credentials,
                                         generated_id(common::IdKind::client));
    ASSERT_TRUE(manager) << manager.error();
    ASSERT_TRUE((*manager)->start());
    EXPECT_EQ((*manager)->session_count(), 1U);

    const auto apply_change = [&](const std::function<void(storage::ServerRecord&)>& mutate) {
        auto current = fixture.repository->servers().get_by_id(enabled.id);
        ASSERT_TRUE(current) << current.error();
        mutate(*current);
        ++current->config_revision;
        current->updated_at_unix_ms = future_updated_at();
        ASSERT_TRUE(fixture.repository->servers().update(*current));
        (*manager)->notify_changed();
        io_context.poll();
    };

    apply_change([](storage::ServerRecord& record) { record.tls_server_name = "renewed.test"; });
    EXPECT_EQ((*manager)->session_count(), 1U);
    apply_change([](storage::ServerRecord& record) { record.credential_ref = "server/psk"; });
    EXPECT_EQ((*manager)->session_count(), 1U);
    apply_change([](storage::ServerRecord& record) {
        record.client_certificate_ref = "server/missing-certificate";
        record.client_private_key_ref = "server/missing-key";
    });
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 0U);
    apply_change([](storage::ServerRecord& record) {
        record.client_certificate_ref = std::nullopt;
        record.client_private_key_ref = std::nullopt;
    });
    io_context.poll();
    EXPECT_EQ((*manager)->session_count(), 1U);
    EXPECT_TRUE(fixture.repository->servers().get_by_id(enabled.id));
    (*manager)->stop();
    io_context.run();
}

} // namespace
} // namespace minitun::daemon
