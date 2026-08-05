#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <minitun/common/id.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/database.hpp>
#include <minitun/storage/state_repository.hpp>

#include "storage_test_support.hpp"

namespace minitun::storage {
namespace {

using test::NativeSqliteDatabase;
using test::TemporaryDatabaseFile;

TEST(StorageDiagnosticsTest, ReportsStateHealthAndWALCounters) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    auto diagnostics = (*repository)->diagnostics();
    ASSERT_TRUE(diagnostics) << diagnostics.error();
    EXPECT_EQ(diagnostics->path, temporary.path_string());
    EXPECT_EQ(diagnostics->schema_version, kCurrentSchemaVersion);
    EXPECT_EQ(diagnostics->journal_mode, "wal");
    EXPECT_EQ(diagnostics->synchronous, "normal");
    EXPECT_TRUE(diagnostics->foreign_keys);
    EXPECT_TRUE(diagnostics->schema_valid);
    EXPECT_TRUE(diagnostics->integrity_ok);
    EXPECT_EQ(diagnostics->integrity_result, "ok");
    EXPECT_GE(diagnostics->page_count, 1);
    EXPECT_GE(diagnostics->wal_log_frames, 0);
    EXPECT_GE(diagnostics->wal_checkpointed_frames, 0);
    EXPECT_EQ(diagnostics->file_mode, 0600U);
}

TEST(StorageDiagnosticsTest, CreatesAtomicStateBackupWithoutOverwriting) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const auto backup_path = temporary.directory() / "state-backup.db";
    auto backed_up = (*repository)->backup_to(backup_path.string());
    ASSERT_TRUE(backed_up) << backed_up.error();

    struct stat status{};
    ASSERT_EQ(::lstat(backup_path.c_str(), &status), 0);
    EXPECT_TRUE(S_ISREG(status.st_mode));
    EXPECT_EQ(status.st_mode & 0777, 0600);

    NativeSqliteDatabase backup{backup_path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(backup.query_int64("SELECT MAX(version) FROM schema_version"), kCurrentSchemaVersion);
    const auto duplicate = (*repository)->backup_to(backup_path.string());
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);
}

TEST(StorageDiagnosticsTest, ReportsAndBacksUpCredentialDatabaseWithoutSecrets) {
    TemporaryDatabaseFile temporary;
    auto credentials = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(credentials) << credentials.error();
    ASSERT_TRUE((*credentials)->put("server/diagnostics", "redacted-token"));

    auto diagnostics = (*credentials)->diagnostics();
    ASSERT_TRUE(diagnostics) << diagnostics.error();
    EXPECT_EQ(diagnostics->schema_version, kCurrentCredentialSchemaVersion);
    EXPECT_EQ(diagnostics->journal_mode, "delete");
    EXPECT_EQ(diagnostics->synchronous, "full");
    EXPECT_TRUE(diagnostics->schema_valid);
    EXPECT_TRUE(diagnostics->integrity_ok);
    EXPECT_EQ(diagnostics->wal_log_frames, -1);

    const auto backup_path = temporary.directory() / "credentials-backup.db";
    auto backed_up = (*credentials)->backup_to(backup_path.string());
    ASSERT_TRUE(backed_up) << backed_up.error();
    NativeSqliteDatabase backup{backup_path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(backup.query_int64("SELECT COUNT(*) FROM credentials"), 1);
    EXPECT_EQ(backup.query_text(
                  "SELECT typeof(secret) FROM credentials WHERE key = 'server/diagnostics'"),
              "blob");
}

TEST(StorageDiagnosticsTest, RejectsInvalidStateRestoreBeforeChangingCurrentDatabase) {
    TemporaryDatabaseFile temporary;
    auto repository = StateRepository::open(temporary.path_string());
    ASSERT_TRUE(repository) << repository.error();

    const auto source_path = temporary.directory() / "invalid-state.db";
    {
        NativeSqliteDatabase source{source_path};
        source.execute("CREATE TABLE unrelated(value TEXT NOT NULL)");
    }
    ASSERT_EQ(::chmod(source_path.c_str(), 0600), 0);

    auto validated = (*repository)->validate_restore_source(source_path.string());
    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.error().code(), common::ErrorCode::database_error);

    auto restored = (*repository)->restore_from(source_path.string());
    ASSERT_FALSE(restored);
    EXPECT_EQ(restored.error().code(), common::ErrorCode::database_error);
    auto diagnostics = (*repository)->diagnostics();
    ASSERT_TRUE(diagnostics) << diagnostics.error();
    EXPECT_TRUE(diagnostics->schema_valid);
    EXPECT_TRUE(diagnostics->integrity_ok);
    EXPECT_EQ(diagnostics->schema_version, kCurrentSchemaVersion);
}

TEST(StorageDiagnosticsTest, RejectsInvalidCredentialRestoreBeforeChangingCurrentDatabase) {
    TemporaryDatabaseFile temporary;
    auto credentials = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(credentials) << credentials.error();
    ASSERT_TRUE((*credentials)->put("server/diagnostics", "token"));

    const auto source_path = temporary.directory() / "invalid-credentials.db";
    {
        NativeSqliteDatabase source{source_path};
        source.execute("CREATE TABLE unrelated(value TEXT NOT NULL)");
    }
    ASSERT_EQ(::chmod(source_path.c_str(), 0600), 0);

    auto validated = (*credentials)->validate_restore_source(source_path.string());
    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.error().code(), common::ErrorCode::unsupported_version);

    auto restored = (*credentials)->restore_from(source_path.string());
    ASSERT_FALSE(restored);
    EXPECT_EQ(restored.error().code(), common::ErrorCode::unsupported_version);
    auto token = (*credentials)->get("server/diagnostics");
    ASSERT_TRUE(token) << token.error();
    EXPECT_EQ(token->view(), "token");
}

} // namespace
} // namespace minitun::storage
