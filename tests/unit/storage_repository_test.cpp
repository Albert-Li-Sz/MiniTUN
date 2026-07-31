#include <atomic>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::storage {
namespace {

[[nodiscard]] common::Id make_id(const common::IdKind kind, const std::uint64_t number) {
    std::ostringstream suffix;
    suffix << std::hex << std::nouppercase << std::setfill('0') << std::setw(32) << number;
    auto parsed = common::Id::parse(std::string{common::id_prefix(kind)} + suffix.str(), kind);
    if (!parsed) {
        throw std::runtime_error("failed to construct deterministic test ID");
    }
    return std::move(*parsed);
}

[[nodiscard]] common::Endpoint make_endpoint(const std::string_view value) {
    auto parsed = common::Endpoint::parse(value);
    if (!parsed) {
        throw std::runtime_error("failed to construct deterministic test endpoint");
    }
    return std::move(*parsed);
}

[[nodiscard]] ServerRecord make_server(const std::uint64_t number,
                                       std::optional<std::string> name) {
    return ServerRecord{
        .id = make_id(common::IdKind::server, number),
        .name = std::move(name),
        .endpoint = make_endpoint("tunnel.example.com:2333"),
        .credential_ref = std::string{"credential-key"},
        .remote_server_id = std::string{"remote-server-id"},
        .desired_state = ServerDesiredState::enabled,
        .actual_state = ServerActualState::online,
        .last_error_code = common::ErrorCode::connection_timeout,
        .last_error_message = std::string{"previous non-sensitive timeout"},
        .reconnect_attempt = 3,
        .latency_ms = 42,
        .created_at_unix_ms = static_cast<std::int64_t>(1'000 + number),
        .updated_at_unix_ms = static_cast<std::int64_t>(2'000 + number),
    };
}

[[nodiscard]] TunnelRecord make_tunnel(const std::uint64_t number, const common::Id& server_id,
                                       const std::uint16_t remote_port,
                                       std::optional<std::string> name = std::nullopt,
                                       const std::string_view remote_host = "0.0.0.0") {
    const std::string remote = std::string{remote_host} + ':' + std::to_string(remote_port);
    return TunnelRecord{
        .id = make_id(common::IdKind::tunnel, number),
        .name = std::move(name),
        .server_id = server_id,
        .protocol = TunnelProtocol::tcp,
        .local_endpoint = make_endpoint("127.0.0.1:22"),
        .remote_endpoint = make_endpoint(remote),
        .desired_state = TunnelDesiredState::active,
        .actual_state = TunnelActualState::active,
        .last_error_code = common::ErrorCode::local_connect_failed,
        .last_error_message = std::string{"previous local failure"},
        .created_at_unix_ms = static_cast<std::int64_t>(3'000 + number),
        .updated_at_unix_ms = static_cast<std::int64_t>(4'000 + number),
    };
}

TEST(ServerRepositoryTest, RoundTripsEveryFieldAndPersistsAcrossReopen) {
    test::TemporaryDatabaseFile temporary;
    const ServerRecord first = make_server(1, "primary'; DROP TABLE servers; --");
    ServerRecord unnamed = make_server(2, std::nullopt);
    unnamed.credential_ref = std::nullopt;
    unnamed.remote_server_id = std::nullopt;
    unnamed.last_error_code = std::nullopt;
    unnamed.last_error_message = std::nullopt;
    unnamed.latency_ms = std::nullopt;
    unnamed.created_at_unix_ms = 500;
    unnamed.updated_at_unix_ms = 600;

    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();

        ASSERT_TRUE((*repository)->servers().create(first));
        ASSERT_TRUE((*repository)->servers().create(unnamed));

        const auto by_id = (*repository)->servers().get_by_id(first.id);
        ASSERT_TRUE(by_id) << by_id.error();
        EXPECT_EQ(*by_id, first);

        const auto by_name = (*repository)->servers().get_by_name(*first.name);
        ASSERT_TRUE(by_name) << by_name.error();
        EXPECT_EQ(*by_name, first);

        const auto listed = (*repository)->servers().list();
        ASSERT_TRUE(listed) << listed.error();
        ASSERT_EQ(listed->size(), 2U);
        EXPECT_EQ((*listed)[0], unnamed);
        EXPECT_EQ((*listed)[1], first);
    }

    auto reopened = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(reopened) << reopened.error();
    const auto restored = (*reopened)->servers().get_by_id(first.id);
    ASSERT_TRUE(restored) << restored.error();
    EXPECT_EQ(*restored, first);
}

TEST(ServerRepositoryTest, UpdatesAtomicallyAndEnforcesUniqueKeys) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    ServerRecord first = make_server(10, "primary");
    ServerRecord second = make_server(11, "backup");
    ASSERT_TRUE((*repository)->servers().create(first));
    ASSERT_TRUE((*repository)->servers().create(second));

    const auto duplicate_id = (*repository)->servers().create(first);
    ASSERT_FALSE(duplicate_id);
    EXPECT_EQ(duplicate_id.error().code(), common::ErrorCode::already_exists);

    ServerRecord duplicate_name = make_server(12, "primary");
    const auto duplicate_name_result = (*repository)->servers().create(duplicate_name);
    ASSERT_FALSE(duplicate_name_result);
    EXPECT_EQ(duplicate_name_result.error().code(), common::ErrorCode::already_exists);

    second.name = first.name;
    second.endpoint = make_endpoint("[2001:db8::10]:2444");
    second.updated_at_unix_ms += 100;
    const auto conflicting_update = (*repository)->servers().update(second);
    ASSERT_FALSE(conflicting_update);
    EXPECT_EQ(conflicting_update.error().code(), common::ErrorCode::already_exists);

    const auto unchanged = (*repository)->servers().get_by_id(second.id);
    ASSERT_TRUE(unchanged) << unchanged.error();
    EXPECT_EQ(unchanged->name, std::optional<std::string>{"backup"});
    EXPECT_EQ(unchanged->endpoint.to_string(), "tunnel.example.com:2333");

    first.endpoint = make_endpoint("[2001:db8::1]:443");
    first.credential_ref = "new-reference";
    first.remote_server_id = "new-remote-id";
    first.actual_state = ServerActualState::backoff;
    first.last_error_code = common::ErrorCode::connection_failed;
    first.last_error_message = "new failure";
    first.reconnect_attempt = 8;
    first.latency_ms = 99;
    first.updated_at_unix_ms += 500;
    ASSERT_TRUE((*repository)->servers().update(first));

    const auto updated = (*repository)->servers().get_by_id(first.id);
    ASSERT_TRUE(updated) << updated.error();
    EXPECT_EQ(*updated, first);
}

TEST(ServerRepositoryTest, EnforcesInputAndConfiguredCountLimits) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string(),
                                            StorageLimits{.max_servers = 2, .max_tunnels = 4});
    ASSERT_TRUE(repository) << repository.error();

    ServerRecord maximum_name = make_server(20, std::string(kMaxNameBytes, 'n'));
    ASSERT_TRUE((*repository)->servers().create(maximum_name));
    ASSERT_TRUE((*repository)->servers().create(make_server(21, std::nullopt)));

    const auto at_capacity = (*repository)->servers().create(make_server(22, "overflow"));
    ASSERT_FALSE(at_capacity);
    EXPECT_EQ(at_capacity.error().code(), common::ErrorCode::resource_exhausted);

    ServerRecord oversized = make_server(23, std::string(kMaxNameBytes + 1U, 'x'));
    const auto oversized_result = (*repository)->servers().update(oversized);
    ASSERT_FALSE(oversized_result);
    EXPECT_EQ(oversized_result.error().code(), common::ErrorCode::invalid_argument);

    std::string invalid_utf8{"bad"};
    invalid_utf8.push_back(static_cast<char>(0xffU));
    ServerRecord malformed = make_server(24, invalid_utf8);
    const auto malformed_result = (*repository)->servers().update(malformed);
    ASSERT_FALSE(malformed_result);
    EXPECT_EQ(malformed_result.error().code(), common::ErrorCode::invalid_argument);

    ServerRecord wrong_kind = make_server(25, "wrong-kind");
    wrong_kind.id = make_id(common::IdKind::tunnel, 25);
    const auto wrong_kind_result = (*repository)->servers().update(wrong_kind);
    ASSERT_FALSE(wrong_kind_result);
    EXPECT_EQ(wrong_kind_result.error().code(), common::ErrorCode::invalid_argument);
}

TEST(TunnelRepositoryTest, EnforcesPerServerRemoteBindingAndForeignKeys) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord first_server = make_server(30, "first");
    const ServerRecord second_server = make_server(31, "second");
    ASSERT_TRUE((*repository)->servers().create(first_server));
    ASSERT_TRUE((*repository)->servers().create(second_server));

    const TunnelRecord first = make_tunnel(30, first_server.id, 6'000, "ssh");
    const TunnelRecord same_binding_other_server = make_tunnel(31, second_server.id, 6'000, "ssh");
    const TunnelRecord same_port_other_host =
        make_tunnel(32, first_server.id, 6'000, "other-host", "127.0.0.1");
    ASSERT_TRUE((*repository)->tunnels().create(first));
    ASSERT_TRUE((*repository)->tunnels().create(same_binding_other_server));
    ASSERT_TRUE((*repository)->tunnels().create(same_port_other_host));

    TunnelRecord duplicate_binding = make_tunnel(33, first_server.id, 6'000, "duplicate");
    const auto duplicate = (*repository)->tunnels().create(duplicate_binding);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);

    TunnelRecord missing_parent =
        make_tunnel(34, make_id(common::IdKind::server, 999), 6'001, "orphan");
    const auto orphan = (*repository)->tunnels().create(missing_parent);
    ASSERT_FALSE(orphan);
    EXPECT_EQ(orphan.error().code(), common::ErrorCode::not_found);

    const auto first_server_tunnels = (*repository)->tunnels().list_by_server(first_server.id);
    ASSERT_TRUE(first_server_tunnels) << first_server_tunnels.error();
    EXPECT_EQ(first_server_tunnels->size(), 2U);

    const auto ambiguous = (*repository)->tunnels().get_by_name("ssh");
    ASSERT_FALSE(ambiguous);
    EXPECT_EQ(ambiguous.error().code(), common::ErrorCode::invalid_argument);
}

TEST(TunnelRepositoryTest, RoundTripsUpdatesTombstonesAndCascadingErase) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord server = make_server(40, "primary");
    ASSERT_TRUE((*repository)->servers().create(server));
    TunnelRecord first = make_tunnel(40, server.id, 6'100, "first");
    const TunnelRecord second = make_tunnel(41, server.id, 6'101, "second");
    ASSERT_TRUE((*repository)->tunnels().create(first));
    ASSERT_TRUE((*repository)->tunnels().create(second));

    const auto loaded = (*repository)->tunnels().get_by_id(first.id);
    ASSERT_TRUE(loaded) << loaded.error();
    EXPECT_EQ(*loaded, first);

    first.local_endpoint = make_endpoint("[::1]:8080");
    first.remote_endpoint = make_endpoint("127.0.0.1:6200");
    first.actual_state = TunnelActualState::failed;
    first.last_error_message = "updated failure";
    first.updated_at_unix_ms += 100;
    ASSERT_TRUE((*repository)->tunnels().update(first));
    const auto updated = (*repository)->tunnels().get_by_id(first.id);
    ASSERT_TRUE(updated) << updated.error();
    EXPECT_EQ(*updated, first);

    ASSERT_TRUE((*repository)->tunnels().mark_removed(first.id, first.updated_at_unix_ms + 1));
    const auto tombstone = (*repository)->tunnels().get_by_id(first.id);
    ASSERT_TRUE(tombstone) << tombstone.error();
    EXPECT_EQ(tombstone->desired_state, TunnelDesiredState::removed);
    EXPECT_EQ(tombstone->actual_state, TunnelActualState::removing);

    ASSERT_TRUE((*repository)->servers().mark_removed(server.id, second.updated_at_unix_ms + 1));
    const auto child_tombstone = (*repository)->tunnels().get_by_id(second.id);
    ASSERT_TRUE(child_tombstone) << child_tombstone.error();
    EXPECT_EQ(child_tombstone->desired_state, TunnelDesiredState::removed);
    EXPECT_EQ(child_tombstone->actual_state, TunnelActualState::removing);

    ASSERT_TRUE((*repository)->servers().erase(server.id));
    const auto cascaded = (*repository)->tunnels().get_by_id(second.id);
    ASSERT_FALSE(cascaded);
    EXPECT_EQ(cascaded.error().code(), common::ErrorCode::not_found);
}

TEST(StorageRepositoryInvariantTest, RejectsTimeRegressionAndPrematurePhysicalErase) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    ServerRecord server = make_server(45, "guarded");
    TunnelRecord tunnel = make_tunnel(45, server.id, 6'150, "guarded");
    ASSERT_TRUE((*repository)->servers().create(server));
    ASSERT_TRUE((*repository)->tunnels().create(tunnel));

    const auto active_tunnel_erase = (*repository)->tunnels().erase(tunnel.id);
    ASSERT_FALSE(active_tunnel_erase);
    EXPECT_EQ(active_tunnel_erase.error().code(), common::ErrorCode::invalid_argument);

    const auto active_server_erase = (*repository)->servers().erase(server.id);
    ASSERT_FALSE(active_server_erase);
    EXPECT_EQ(active_server_erase.error().code(), common::ErrorCode::invalid_argument);

    ServerRecord stale_server = server;
    stale_server.endpoint = make_endpoint("new.example.com:2444");
    --stale_server.updated_at_unix_ms;
    const auto stale_server_update = (*repository)->servers().update(stale_server);
    ASSERT_FALSE(stale_server_update);
    EXPECT_EQ(stale_server_update.error().code(), common::ErrorCode::invalid_argument);

    TunnelRecord stale_tunnel = tunnel;
    stale_tunnel.local_endpoint = make_endpoint("127.0.0.1:8080");
    --stale_tunnel.updated_at_unix_ms;
    const auto stale_tunnel_update = (*repository)->tunnels().update(stale_tunnel);
    ASSERT_FALSE(stale_tunnel_update);
    EXPECT_EQ(stale_tunnel_update.error().code(), common::ErrorCode::invalid_argument);

    ServerRecord rewritten_creation_time = server;
    ++rewritten_creation_time.created_at_unix_ms;
    const auto rewritten_server = (*repository)->servers().update(rewritten_creation_time);
    ASSERT_FALSE(rewritten_server);
    EXPECT_EQ(rewritten_server.error().code(), common::ErrorCode::invalid_argument);

    const auto stale_server_removal =
        (*repository)->servers().mark_removed(server.id, server.updated_at_unix_ms - 1);
    ASSERT_FALSE(stale_server_removal);
    EXPECT_EQ(stale_server_removal.error().code(), common::ErrorCode::invalid_argument);

    const auto stale_tunnel_removal =
        (*repository)->tunnels().mark_removed(tunnel.id, tunnel.updated_at_unix_ms - 1);
    ASSERT_FALSE(stale_tunnel_removal);
    EXPECT_EQ(stale_tunnel_removal.error().code(), common::ErrorCode::invalid_argument);

    server.desired_state = ServerDesiredState::removed;
    server.actual_state = ServerActualState::disabled;
    ++server.updated_at_unix_ms;
    ASSERT_TRUE((*repository)->servers().update(server));

    const auto live_child_cascade = (*repository)->servers().erase(server.id);
    ASSERT_FALSE(live_child_cascade);
    EXPECT_EQ(live_child_cascade.error().code(), common::ErrorCode::invalid_argument);

    const auto preserved_server = (*repository)->servers().get_by_id(server.id);
    ASSERT_TRUE(preserved_server) << preserved_server.error();
    const auto preserved_tunnel = (*repository)->tunnels().get_by_id(tunnel.id);
    ASSERT_TRUE(preserved_tunnel) << preserved_tunnel.error();
    EXPECT_EQ(preserved_tunnel->desired_state, TunnelDesiredState::active);
}

TEST(StorageTransactionTest, CommitsMultipleRepositoriesAtomically) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord server = make_server(50, "atomic");
    const TunnelRecord tunnel = make_tunnel(50, server.id, 6'200, "atomic");
    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    ASSERT_TRUE((*repository)->servers().create(server, *transaction));
    ASSERT_TRUE((*repository)->tunnels().create(tunnel, *transaction));
    ASSERT_TRUE(transaction->commit());

    EXPECT_TRUE((*repository)->servers().get_by_id(server.id));
    EXPECT_TRUE((*repository)->tunnels().get_by_id(tunnel.id));
}

TEST(StorageTransactionTest, AbandonmentAndConstraintFailureRollBackAllWrites) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const ServerRecord abandoned = make_server(60, "abandoned");
    {
        auto transaction = (*repository)->begin_transaction();
        ASSERT_TRUE(transaction) << transaction.error();
        ASSERT_TRUE((*repository)->servers().create(abandoned, *transaction));

        const auto nested = (*repository)->begin_transaction();
        ASSERT_FALSE(nested);
        EXPECT_EQ(nested.error().code(), common::ErrorCode::invalid_argument);
    }
    EXPECT_EQ((*repository)->servers().get_by_id(abandoned.id).error().code(),
              common::ErrorCode::not_found);

    const ServerRecord first = make_server(61, "duplicate-name");
    ServerRecord duplicate = make_server(62, "duplicate-name");
    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    ASSERT_TRUE((*repository)->servers().create(first, *transaction));
    const auto conflict = (*repository)->servers().create(duplicate, *transaction);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code(), common::ErrorCode::already_exists);
    EXPECT_TRUE(transaction->failed());

    const auto committed = transaction->commit();
    ASSERT_FALSE(committed);
    EXPECT_EQ(committed.error().code(), common::ErrorCode::already_exists);
    EXPECT_EQ((*repository)->servers().get_by_id(first.id).error().code(),
              common::ErrorCode::not_found);
}

TEST(StorageTransactionTest, OtherConnectionCannotSeeUncommittedWrites) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();
    test::NativeSqliteDatabase observer{temporary.path()};

    const ServerRecord server = make_server(70, "isolated");
    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    ASSERT_TRUE((*repository)->servers().create(server, *transaction));

    EXPECT_EQ(observer.query_int64("SELECT COUNT(*) FROM servers"), 0);
    ASSERT_TRUE(transaction->commit());
    EXPECT_EQ(observer.query_int64("SELECT COUNT(*) FROM servers"), 1);
}

TEST(StorageTransactionTest, SerializesConcurrentWritesOnOneConnection) {
    test::TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    constexpr std::size_t kWorkerCount = 8;
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkerCount);
    for (std::size_t index = 0; index < kWorkerCount; ++index) {
        workers.emplace_back([&repository, &failures, index] {
            auto created =
                (*repository)
                    ->servers()
                    .create(make_server(100U + index, "concurrent-" + std::to_string(index)));
            if (!created) {
                failures.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0U);
    const auto servers = (*repository)->servers().list();
    ASSERT_TRUE(servers) << servers.error();
    EXPECT_EQ(servers->size(), kWorkerCount);
}

} // namespace
} // namespace minitun::storage
