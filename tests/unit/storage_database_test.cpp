#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/storage/database.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::storage {
namespace {

using test::NativeSqliteDatabase;
using test::TemporaryDatabaseFile;

[[nodiscard]] common::Id require_id(const std::string_view text,
                                    const common::IdKind expected_kind) {
    auto parsed = common::Id::parse(text, expected_kind);
    if (!parsed) {
        throw std::runtime_error("invalid fixed test ID");
    }
    return std::move(*parsed);
}

[[nodiscard]] common::Endpoint require_endpoint(const std::string_view text) {
    auto parsed = common::Endpoint::parse(text);
    if (!parsed) {
        throw std::runtime_error("invalid fixed test endpoint");
    }
    return std::move(*parsed);
}

[[nodiscard]] ServerRecord sample_server() {
    return ServerRecord{
        .id = require_id("srv_00000000000000000000000000000001", common::IdKind::server),
        .name = std::string{"primary"},
        .endpoint = require_endpoint("127.0.0.1:2333"),
        .credential_ref = std::nullopt,
        .remote_server_id = std::nullopt,
        .desired_state = ServerDesiredState::enabled,
        .actual_state = ServerActualState::not_authenticated,
        .last_error_code = std::nullopt,
        .last_error_message = std::nullopt,
        .reconnect_attempt = 0,
        .latency_ms = std::nullopt,
        .created_at_unix_ms = 1'000,
        .updated_at_unix_ms = 1'000,
    };
}

[[nodiscard]] TunnelRecord sample_tunnel_with_missing_server() {
    return TunnelRecord{
        .id = require_id("tun_00000000000000000000000000000001", common::IdKind::tunnel),
        .name = std::string{"ssh"},
        .server_id = require_id("srv_00000000000000000000000000000002", common::IdKind::server),
        .protocol = TunnelProtocol::tcp,
        .local_endpoint = require_endpoint("127.0.0.1:22"),
        .remote_endpoint = require_endpoint("0.0.0.0:6000"),
        .desired_state = TunnelDesiredState::active,
        .actual_state = TunnelActualState::pending,
        .last_error_code = std::nullopt,
        .last_error_message = std::nullopt,
        .created_at_unix_ms = 1'000,
        .updated_at_unix_ms = 1'000,
        .last_synced_at_unix_ms = std::nullopt,
    };
}

TEST(StorageDatabaseTest, FreshDatabaseMigratesCompleteVersionFiveSchema) {
    TemporaryDatabaseFile temporary;
    auto database = Database::open(temporary.path_string());

    ASSERT_TRUE(database) << database.error();
    struct stat status{};
    ASSERT_EQ(::lstat(temporary.path_string().c_str(), &status), 0);
    EXPECT_TRUE(S_ISREG(status.st_mode));
    EXPECT_EQ(status.st_uid, ::geteuid());
    EXPECT_EQ(status.st_nlink, 1);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    const auto version = (*database)->schema_version();
    ASSERT_TRUE(version) << version.error();
    EXPECT_EQ(*version, kCurrentSchemaVersion);

    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM sqlite_master "
                                "WHERE type = 'table' AND name IN ("
                                "'schema_version', 'servers', 'tunnels', 'daemon_identity')"),
              4);
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM pragma_table_info('schema_version')"), 2);
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM pragma_table_info('servers')"), 19);
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM pragma_table_info('tunnels')"), 17);
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM pragma_table_info('daemon_identity')"), 2);
    EXPECT_EQ(
        probe.query_int64("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name IN ("
                          "'idx_servers_reconcile', 'idx_tunnels_reconcile', 'idx_tunnels_name')"),
        3);
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM schema_version"), 5);
    EXPECT_EQ(probe.query_int64("SELECT MAX(version) FROM schema_version"), kCurrentSchemaVersion);
    EXPECT_GE(probe.query_int64("SELECT MIN(applied_at) FROM schema_version"), 0);
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM daemon_identity"), 0);
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM pragma_foreign_key_list('tunnels') "
                                "WHERE \"table\" = 'servers' AND \"from\" = 'server_id' "
                                "AND \"to\" = 'id' AND on_delete = 'CASCADE'"),
              1);
}

TEST(StorageDatabaseTest, RejectsSymbolicLinksHardLinksAndWritableParentDirectories) {
    TemporaryDatabaseFile temporary;
    const auto target = temporary.directory() / "target.db";
    test::write_binary_file(target, "preserve-me");

    const auto symbolic_link = temporary.directory() / "symbolic.db";
    std::filesystem::create_symlink(target, symbolic_link);
    const auto symbolic = Database::open(symbolic_link.string());
    ASSERT_FALSE(symbolic);
    EXPECT_EQ(symbolic.error().code(), common::ErrorCode::permission_denied);
    EXPECT_EQ(test::read_binary_file(target), "preserve-me");

    const auto hard_link = temporary.directory() / "hard.db";
    std::filesystem::create_hard_link(target, hard_link);
    const auto hard = Database::open(hard_link.string());
    ASSERT_FALSE(hard);
    EXPECT_EQ(hard.error().code(), common::ErrorCode::permission_denied);
    EXPECT_EQ(test::read_binary_file(target), "preserve-me");

    ASSERT_EQ(::chmod(temporary.directory().c_str(), 0770), 0);
    const auto writable_parent = Database::open(temporary.path_string());
    ASSERT_FALSE(writable_parent);
    EXPECT_EQ(writable_parent.error().code(), common::ErrorCode::permission_denied);
    ASSERT_EQ(::chmod(temporary.directory().c_str(), 0700), 0);
}

TEST(StorageDatabaseTest, EnablesWalAndRequiredConnectionPolicy) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());

    ASSERT_TRUE(repository) << repository.error();
    NativeSqliteDatabase probe{temporary.path()};
    EXPECT_EQ(probe.query_text("PRAGMA journal_mode"), "wal");
    EXPECT_EQ(kDatabaseBusyTimeoutMilliseconds, 5'000);
    EXPECT_EQ(kWalAutoCheckpointPages, 1'000);
    EXPECT_EQ(kWalJournalSizeLimitBytes, 16 * 1024 * 1024);

    const auto missing_parent =
        (*repository)->tunnels().create(sample_tunnel_with_missing_server());
    ASSERT_FALSE(missing_parent);
    EXPECT_EQ(missing_parent.error().code(), common::ErrorCode::not_found);
}

TEST(StorageDatabaseTest, ExplicitCheckpointRejectsActiveTransactionsAndFlushesWal) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    ASSERT_TRUE((*repository)->checkpoint());
    const std::filesystem::path immutable_path{"file:" + temporary.path_string() + "?immutable=1"};
    const int immutable_flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI;
    {
        NativeSqliteDatabase main_file_only{immutable_path, immutable_flags};
        EXPECT_EQ(main_file_only.query_int64("SELECT COUNT(*) FROM servers"), 0);
    }

    ASSERT_TRUE((*repository)->servers().create(sample_server()));
    {
        NativeSqliteDatabase main_file_only{immutable_path, immutable_flags};
        EXPECT_EQ(main_file_only.query_int64("SELECT COUNT(*) FROM servers"), 0);
    }

    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    const auto active_checkpoint = (*repository)->checkpoint();
    ASSERT_FALSE(active_checkpoint);
    EXPECT_EQ(active_checkpoint.error().code(), common::ErrorCode::invalid_argument);
    ASSERT_TRUE(transaction->rollback());

    ASSERT_TRUE((*repository)->checkpoint());
    NativeSqliteDatabase main_file_only{immutable_path, immutable_flags};
    EXPECT_EQ(main_file_only.query_int64("SELECT COUNT(*) FROM servers"), 1);
}

TEST(StorageDatabaseTest, ReopeningCurrentSchemaIsIdempotentAndPreservesData) {
    TemporaryDatabaseFile temporary;
    std::int64_t first_applied_at = 0;
    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        ASSERT_TRUE((*repository)->servers().create(sample_server()));

        NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
        first_applied_at =
            probe.query_int64("SELECT applied_at FROM schema_version WHERE version = 1");
    }

    auto reopened = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(reopened) << reopened.error();
    const auto restored = (*reopened)->servers().get_by_id(sample_server().id);
    ASSERT_TRUE(restored) << restored.error();
    EXPECT_EQ(*restored, sample_server());

    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM schema_version"), 5);
    EXPECT_EQ(probe.query_int64("SELECT applied_at FROM schema_version WHERE version = 1"),
              first_applied_at);
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM servers"), 1);
}

TEST(StorageDatabaseTest, RejectsVersionOneDatabasesFromPreV1Releases) {
    TemporaryDatabaseFile temporary;
    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        ASSERT_TRUE((*repository)->servers().create(sample_server()));
    }
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute("DELETE FROM schema_version WHERE version IN (2, 3, 4, 5)");
    }

    const auto rejected = StateRepository::open(temporary.path_string());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), common::ErrorCode::unsupported_version);
    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM schema_version"), 1);
}

TEST(StorageDatabaseTest, RejectsVersionTwoDatabasesFromPreV1Releases) {
    TemporaryDatabaseFile temporary;
    TunnelRecord expected = sample_tunnel_with_missing_server();
    expected.server_id = sample_server().id;
    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        ASSERT_TRUE((*repository)->servers().create(sample_server()));
        ASSERT_TRUE((*repository)->tunnels().create(expected));
    }
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute("DELETE FROM schema_version WHERE version IN (3, 4, 5)");
    }

    const auto rejected = StateRepository::open(temporary.path_string());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), common::ErrorCode::unsupported_version);
}

TEST(StorageDatabaseTest, RejectsVersionThreeDatabasesFromV041) {
    TemporaryDatabaseFile temporary;
    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        ASSERT_TRUE((*repository)->servers().create(sample_server()));
        TunnelRecord expected_tunnel = sample_tunnel_with_missing_server();
        expected_tunnel.server_id = sample_server().id;
        ASSERT_TRUE((*repository)->tunnels().create(expected_tunnel));
    }
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute("DELETE FROM schema_version WHERE version IN (4, 5)");
    }

    const auto rejected = StateRepository::open(temporary.path_string());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), common::ErrorCode::unsupported_version);
    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM schema_version"), 3);
}

TEST(StorageDatabaseTest, PersistsOneStableDaemonClientIdentity) {
    TemporaryDatabaseFile temporary;
    std::string first;
    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        const auto identity = (*repository)->client_id();
        ASSERT_TRUE(identity) << identity.error();
        EXPECT_EQ(identity->kind(), common::IdKind::client);
        first = identity->str();
        EXPECT_EQ((*repository)->client_id()->str(), first);
    }

    auto reopened = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(reopened) << reopened.error();
    const auto identity = (*reopened)->client_id();
    ASSERT_TRUE(identity) << identity.error();
    EXPECT_EQ(identity->str(), first);

    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM daemon_identity"), 1);
    EXPECT_EQ(probe.query_text("SELECT client_id FROM daemon_identity"), first);
}

TEST(StorageDatabaseTest, RejectsFutureSchemaWithoutChangingItsData) {
    TemporaryDatabaseFile temporary;
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute(
            "CREATE TABLE schema_version(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);"
            "INSERT INTO schema_version(version, applied_at) VALUES(6, 1234);"
            "CREATE TABLE future_data(value TEXT NOT NULL);"
            "INSERT INTO future_data(value) VALUES('preserve-me');");
    }

    const auto opened = Database::open(temporary.path_string());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), common::ErrorCode::unsupported_version);

    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_int64("SELECT version FROM schema_version"), 6);
    EXPECT_EQ(probe.query_int64("SELECT applied_at FROM schema_version"), 1'234);
    EXPECT_EQ(probe.query_text("SELECT value FROM future_data"), "preserve-me");
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM sqlite_master "
                                "WHERE type = 'table' AND name IN ('servers', 'tunnels')"),
              0);
}

TEST(StorageDatabaseTest, RejectsUnversionedNonEmptyDatabaseWithoutChangingIt) {
    TemporaryDatabaseFile temporary;
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute("CREATE TABLE legacy_data(id INTEGER PRIMARY KEY, value TEXT NOT NULL);"
                        "INSERT INTO legacy_data(id, value) VALUES(7, 'preserve-me');");
    }

    const auto opened = Database::open(temporary.path_string());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), common::ErrorCode::database_error);

    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_int64("SELECT id FROM legacy_data"), 7);
    EXPECT_EQ(probe.query_text("SELECT value FROM legacy_data"), "preserve-me");
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM sqlite_master "
                                "WHERE type = 'table' AND name = 'schema_version'"),
              0);
}

TEST(StorageDatabaseTest, RejectsViewOnlyUnversionedDatabaseWithoutChangingIt) {
    TemporaryDatabaseFile temporary;
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute("CREATE VIEW legacy_view AS SELECT 'pre-existing' AS marker");
    }

    const auto opened = Database::open(temporary.path_string());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), common::ErrorCode::database_error);

    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_text("SELECT marker FROM legacy_view"), "pre-existing");
    EXPECT_EQ(
        probe.query_int64(
            "SELECT COUNT(*) FROM sqlite_master WHERE type = 'view' AND name = 'legacy_view'"),
        1);
    EXPECT_EQ(probe.query_int64(
                  "SELECT COUNT(*) FROM sqlite_master "
                  "WHERE type = 'table' AND name IN ('schema_version', 'servers', 'tunnels')"),
              0);
}

TEST(StorageDatabaseTest, RejectsCurrentSchemaDriftWithoutChangingIt) {
    TemporaryDatabaseFile temporary;
    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        ASSERT_TRUE((*repository)->servers().create(sample_server()));
    }
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute("DROP INDEX idx_servers_reconcile;"
                        "CREATE INDEX idx_servers_reconcile ON servers(name)");
    }

    const auto opened = Database::open(temporary.path_string());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), common::ErrorCode::database_error);

    NativeSqliteDatabase probe{temporary.path()};
    EXPECT_EQ(probe.query_text("SELECT sql FROM sqlite_master "
                               "WHERE type = 'index' AND name = 'idx_servers_reconcile'"),
              "CREATE INDEX idx_servers_reconcile ON servers(name)");
    EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM servers"), 1);
}

TEST(StorageDatabaseTest, RejectsEveryCurrentSchemaAndHistoryDriftClass) {
    struct Drift final {
        std::string name;
        std::string mutation;
    };
    const std::vector<Drift> cases{
        {"missing-index", "DROP INDEX idx_servers_reconcile"},
        {"server-index-definition",
         "DROP INDEX idx_servers_reconcile; CREATE INDEX idx_servers_reconcile ON servers(name)"},
        {"tunnel-index-definition",
         "DROP INDEX idx_tunnels_reconcile; CREATE INDEX idx_tunnels_reconcile ON tunnels(name)"},
        {"name-index-definition",
         "DROP INDEX idx_tunnels_name; CREATE INDEX idx_tunnels_name ON tunnels(name)"},
        {"identity-definition",
         "DROP TABLE daemon_identity; CREATE TABLE daemon_identity(singleton INTEGER, client_id "
         "TEXT)"},
        {"unexpected-table", "CREATE TABLE unexpected(value TEXT)"},
        {"unexpected-view", "CREATE VIEW unexpected AS SELECT 1 AS value"},
        {"unexpected-trigger",
         "CREATE TRIGGER unexpected AFTER INSERT ON servers BEGIN SELECT 1; END"},
        {"unexpected-index", "CREATE INDEX unexpected ON servers(endpoint)"},
        {"server-column", "ALTER TABLE servers ADD COLUMN unexpected TEXT"},
        {"tunnel-column", "ALTER TABLE tunnels ADD COLUMN unexpected TEXT"},
        {"history-gap", "DELETE FROM schema_version WHERE version = 2"},
        {"history-empty", "DELETE FROM schema_version"},
        {"history-type",
         "DROP TABLE schema_version; CREATE TABLE schema_version(version TEXT, applied_at "
         "INTEGER); INSERT INTO schema_version VALUES('one', 1)"},
        {"foreign-key-violation",
         "PRAGMA foreign_keys = OFF; PRAGMA ignore_check_constraints = ON; UPDATE tunnels SET "
         "server_id = 'srv_00000000000000000000000000000099'"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        TemporaryDatabaseFile temporary;
        {
            auto repository = StateRepository::open(temporary.path_string());
            ASSERT_TRUE(repository) << repository.error();
            const auto server = sample_server();
            auto tunnel = sample_tunnel_with_missing_server();
            tunnel.server_id = server.id;
            ASSERT_TRUE((*repository)->servers().create(server));
            ASSERT_TRUE((*repository)->tunnels().create(tunnel));
            ASSERT_TRUE((*repository)->checkpoint());
        }
        {
            NativeSqliteDatabase fixture{temporary.path()};
            fixture.execute(item.mutation);
        }
        const auto opened = Database::open(temporary.path_string());
        ASSERT_FALSE(opened);
        EXPECT_EQ(opened.error().code(), common::ErrorCode::database_error) << opened.error();
    }
}

TEST(StorageDatabaseTest, MigratesVersionFourInPlaceAndRejectsPartialMigrationObjects) {
    {
        // A v1.0-era schema v4 database migrates to v5 in place.
        TemporaryDatabaseFile temporary;
        {
            auto repository = StateRepository::open(temporary.path_string());
            ASSERT_TRUE(repository) << repository.error();
            ASSERT_TRUE((*repository)->servers().create(sample_server()));
        }
        {
            NativeSqliteDatabase fixture{temporary.path()};
            fixture.execute("DELETE FROM schema_version WHERE version = 5");
        }
        auto migrated = Database::open(temporary.path_string());
        ASSERT_TRUE(migrated) << migrated.error();
        EXPECT_EQ(*(*migrated)->schema_version(), 5);
        NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
        EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM schema_version"), 5);
        EXPECT_EQ(probe.query_int64("SELECT COUNT(*) FROM servers"), 1);
    }
    {
        // A leftover v4 staging table from an interrupted migration is
        // rejected instead of being silently overwritten.
        TemporaryDatabaseFile temporary;
        {
            auto repository = StateRepository::open(temporary.path_string());
            ASSERT_TRUE(repository) << repository.error();
        }
        {
            NativeSqliteDatabase fixture{temporary.path()};
            fixture.execute("CREATE TABLE tunnels_v4 AS SELECT * FROM tunnels;"
                            "DELETE FROM schema_version WHERE version = 5");
        }
        const auto rejected = Database::open(temporary.path_string());
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code(), common::ErrorCode::database_error);
    }
    {
        // Pre-v1.0 schemas are rejected even when their tables already carry
        // the columns later versions introduced.
        TemporaryDatabaseFile temporary;
        {
            auto repository = StateRepository::open(temporary.path_string());
            ASSERT_TRUE(repository) << repository.error();
        }
        {
            NativeSqliteDatabase fixture{temporary.path()};
            fixture.execute("DELETE FROM schema_version WHERE version IN (4, 5)");
        }
        const auto rejected = Database::open(temporary.path_string());
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code(), common::ErrorCode::unsupported_version);
    }
}

TEST(StorageDatabaseTest, RejectsConstraintViolatingRowsWithoutChangingThem) {
    TemporaryDatabaseFile temporary;
    {
        auto repository = StateRepository::open(temporary.path_string());
        ASSERT_TRUE(repository) << repository.error();
        ASSERT_TRUE((*repository)->servers().create(sample_server()));
    }
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute("PRAGMA ignore_check_constraints = ON");
        fixture.execute("UPDATE servers SET desired_state = 'invalid-state'");
    }

    const auto opened = Database::open(temporary.path_string());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), common::ErrorCode::database_error);

    NativeSqliteDatabase probe{temporary.path()};
    EXPECT_EQ(probe.query_text("SELECT desired_state FROM servers"), "invalid-state");
}

TEST(StorageDatabaseTest, CorruptDatabaseIsRejectedWithoutBeingRecreated) {
    TemporaryDatabaseFile temporary;
    const std::string corrupt_contents =
        "this is deliberately not a SQLite database and must remain untouched";
    test::write_binary_file(temporary.path(), corrupt_contents);

    const auto opened = Database::open(temporary.path_string());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code(), common::ErrorCode::database_error);
    EXPECT_EQ(test::read_binary_file(temporary.path()), corrupt_contents);
}

TEST(StorageDatabaseTest, ExplicitTransactionCommitPersistsWrites) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    ASSERT_TRUE((*repository)->servers().create(sample_server(), *transaction));
    ASSERT_TRUE(transaction->active());
    ASSERT_TRUE(transaction->commit());
    EXPECT_FALSE(transaction->active());

    const auto persisted = (*repository)->servers().get_by_id(sample_server().id);
    ASSERT_TRUE(persisted) << persisted.error();
    EXPECT_EQ(*persisted, sample_server());
}

TEST(StorageDatabaseTest, UncommittedTransactionIsRolledBackByDestruction) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    {
        auto transaction = (*repository)->begin_transaction();
        ASSERT_TRUE(transaction) << transaction.error();
        ASSERT_TRUE((*repository)->servers().create(sample_server(), *transaction));
    }

    const auto missing = (*repository)->servers().get_by_id(sample_server().id);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), common::ErrorCode::not_found);
}

TEST(StorageDatabaseTest, NestedTransactionIsRejectedWithoutEndingOuterTransaction) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    auto outer = (*repository)->begin_transaction();
    ASSERT_TRUE(outer) << outer.error();
    const auto nested = (*repository)->begin_transaction();
    ASSERT_FALSE(nested);
    EXPECT_EQ(nested.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_TRUE(outer->active());
    ASSERT_TRUE(outer->rollback());
}

TEST(StorageDatabaseTest, ActiveTransactionsBlockMaintenanceAndTerminalCallsAreSingleUse) {
    TemporaryDatabaseFile temporary;
    TemporaryDatabaseFile source_file;
    auto database = Database::open(temporary.path_string());
    auto source = Database::open(source_file.path_string());
    ASSERT_TRUE(database) << database.error();
    ASSERT_TRUE(source) << source.error();
    const auto backup = temporary.directory() / "active-backup.db";

    auto transaction = (*database)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    EXPECT_EQ((*database)->backup_to(backup.string()).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*database)->validate_restore_source(source_file.path_string()).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*database)->restore_from(source_file.path_string()).error().code(),
              common::ErrorCode::invalid_argument);
    ASSERT_TRUE(transaction->rollback());
    EXPECT_FALSE(transaction->active());
    EXPECT_EQ(transaction->rollback().error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(transaction->commit().error().code(), common::ErrorCode::invalid_argument);

    auto committed = (*database)->begin_transaction();
    ASSERT_TRUE(committed) << committed.error();
    ASSERT_TRUE(committed->commit());
    EXPECT_FALSE(committed->active());
    EXPECT_EQ(committed->commit().error().code(), common::ErrorCode::invalid_argument);
}

TEST(StorageDatabaseTest, RestoresValidatedStateAndRejectsUnsafeBackupPaths) {
    TemporaryDatabaseFile live_file;
    TemporaryDatabaseFile source_file;
    auto live = StateRepository::open(live_file.path_string());
    auto source = StateRepository::open(source_file.path_string());
    ASSERT_TRUE(live) << live.error();
    ASSERT_TRUE(source) << source.error();
    ASSERT_TRUE((*source)->servers().create(sample_server()));
    ASSERT_TRUE((*source)->checkpoint());

    ASSERT_TRUE((*live)->validate_restore_source(source_file.path_string()));
    ASSERT_TRUE((*live)->restore_from(source_file.path_string()));
    ASSERT_TRUE((*live)->servers().get_by_id(sample_server().id));

    EXPECT_EQ((*live)->backup_to("").error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ((*live)->backup_to(live_file.path_string()).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*live)->backup_to(std::string(kMaxDatabasePathBytes + 1U, 'x')).error().code(),
              common::ErrorCode::invalid_argument);
    EXPECT_EQ((*live)
                  ->backup_to((live_file.directory() / "missing" / "backup.db").string())
                  .error()
                  .code(),
              common::ErrorCode::not_found);
    EXPECT_EQ((*live)->validate_restore_source(live_file.path_string()).error().code(),
              common::ErrorCode::invalid_argument);
}

TEST(StorageDatabaseTest, DiagnosticsReportsMissingBackingPathWithoutUsingStaleState) {
    TemporaryDatabaseFile temporary;
    auto database = Database::open(temporary.path_string());
    ASSERT_TRUE(database) << database.error();
    ASSERT_TRUE(std::filesystem::remove(temporary.path()));
    const auto diagnostics = (*database)->diagnostics();
    ASSERT_FALSE(diagnostics);
    EXPECT_EQ(diagnostics.error().code(), common::ErrorCode::database_error);
}

TEST(StorageDatabaseTest, AnotherConnectionCannotSeeUncommittedWrites) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();
    NativeSqliteDatabase observer{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};

    auto transaction = (*repository)->begin_transaction();
    ASSERT_TRUE(transaction) << transaction.error();
    ASSERT_TRUE((*repository)->servers().create(sample_server(), *transaction));
    EXPECT_EQ(observer.query_int64("SELECT COUNT(*) FROM servers"), 0);

    ASSERT_TRUE(transaction->commit());
    EXPECT_EQ(observer.query_int64("SELECT COUNT(*) FROM servers"), 1);
}

TEST(StorageDatabaseTest, RejectsInvalidStorageLimitsBeforeCreatingDatabase) {
    TemporaryDatabaseFile temporary;

    const auto no_servers = StateRepository::open(
        temporary.path_string(), StorageLimits{.max_servers = 0, .max_tunnels = 1});
    ASSERT_FALSE(no_servers);
    EXPECT_EQ(no_servers.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(temporary.path()));

    const auto no_tunnels = StateRepository::open(
        temporary.path_string(), StorageLimits{.max_servers = 1, .max_tunnels = 0});
    ASSERT_FALSE(no_tunnels);
    EXPECT_EQ(no_tunnels.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(temporary.path()));

    if constexpr (std::numeric_limits<std::size_t>::max() >
                  static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        const std::size_t oversized =
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
        const auto excessive = StateRepository::open(
            temporary.path_string(), StorageLimits{.max_servers = oversized, .max_tunnels = 1});
        ASSERT_FALSE(excessive);
        EXPECT_EQ(excessive.error().code(), common::ErrorCode::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(temporary.path()));
    }
}

} // namespace
} // namespace minitun::storage
