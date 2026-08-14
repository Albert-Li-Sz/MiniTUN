#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/database.hpp>

#include "storage_test_support.hpp"

namespace minitun::storage {
namespace {

using test::NativeSqliteDatabase;
using test::TemporaryDatabaseFile;

TEST(CredentialStoreTest, PersistsUpdatesAndRemovesSecretsInPrivateFile) {
    TemporaryDatabaseFile temporary;
    auto store = SqliteCredentialStore::open(temporary.path_string());

    ASSERT_TRUE(store) << store.error();
    struct stat status{};
    ASSERT_EQ(::stat(temporary.path_string().c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);

    ASSERT_TRUE((*store)->put("server/primary", "first-token"));
    auto first = (*store)->get("server/primary");
    ASSERT_TRUE(first) << first.error();
    EXPECT_EQ(first->view(), "first-token");

    ASSERT_TRUE((*store)->put("server/primary", "replacement-token"));
    auto replacement = (*store)->get("server/primary");
    ASSERT_TRUE(replacement) << replacement.error();
    EXPECT_EQ(replacement->view(), "replacement-token");

    ASSERT_TRUE((*store)->remove("server/primary"));
    ASSERT_TRUE((*store)->remove("server/primary"));
    const auto missing = (*store)->get("server/primary");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), common::ErrorCode::not_found);
}

TEST(CredentialStoreTest, RejectsInvalidKeysAndSecretsWithoutEchoingThem) {
    TemporaryDatabaseFile temporary;
    auto store = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(store) << store.error();

    const auto empty_key = (*store)->put("", "token");
    ASSERT_FALSE(empty_key);
    EXPECT_EQ(empty_key.error().code(), common::ErrorCode::invalid_argument);

    const auto empty_secret = (*store)->put("server/key", "");
    ASSERT_FALSE(empty_secret);
    EXPECT_EQ(empty_secret.error().code(), common::ErrorCode::invalid_argument);

    const std::string secret_with_nul{"private\0value", 13U};
    const auto nul_secret = (*store)->put("server/key", secret_with_nul);
    ASSERT_FALSE(nul_secret);
    EXPECT_EQ(nul_secret.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(nul_secret.error().message().find("private"), std::string::npos);
}

TEST(CredentialStoreTest, RejectsFutureSchemaWithoutChangingIt) {
    TemporaryDatabaseFile temporary;
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute("PRAGMA user_version = 2;"
                        "CREATE TABLE future_credentials(marker TEXT NOT NULL);"
                        "INSERT INTO future_credentials(marker) VALUES('preserve-me');");
    }

    const auto store = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_FALSE(store);
    EXPECT_EQ(store.error().code(), common::ErrorCode::unsupported_version);

    NativeSqliteDatabase probe{temporary.path(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
    EXPECT_EQ(probe.query_text("SELECT marker FROM future_credentials"), "preserve-me");
}

TEST(CredentialStoreTest, SerializesConcurrentAccess) {
    TemporaryDatabaseFile temporary;
    auto store = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(store) << store.error();

    constexpr std::size_t kThreadCount = 8U;
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (std::size_t index = 0; index < kThreadCount; ++index) {
        workers.emplace_back([&store, index] {
            const std::string key = "server/" + std::to_string(index);
            const std::string secret = "token-" + std::to_string(index);
            EXPECT_TRUE((*store)->put(key, secret));
            auto loaded = (*store)->get(key);
            ASSERT_TRUE(loaded) << loaded.error();
            EXPECT_EQ(loaded->view(), secret);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

TEST(CredentialStoreTest, RejectsSymbolicLinksAndWritableParentDirectories) {
    TemporaryDatabaseFile temporary;
    const auto target = temporary.directory() / "target.db";
    test::write_binary_file(target, "preserve-me");
    const auto symbolic_link = temporary.directory() / "credentials-link.db";
    std::filesystem::create_symlink(target, symbolic_link);

    const auto symbolic = SqliteCredentialStore::open(symbolic_link.string());
    ASSERT_FALSE(symbolic);
    EXPECT_EQ(symbolic.error().code(), common::ErrorCode::permission_denied);
    EXPECT_EQ(test::read_binary_file(target), "preserve-me");

    ASSERT_EQ(::chmod(temporary.directory().c_str(), 0770), 0);
    const auto writable_parent = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_FALSE(writable_parent);
    EXPECT_EQ(writable_parent.error().code(), common::ErrorCode::permission_denied);
    ASSERT_EQ(::chmod(temporary.directory().c_str(), 0700), 0);
}

TEST(CredentialStoreTest, RejectsAllInvalidPathsKeysAndSecrets) {
    EXPECT_EQ(SqliteCredentialStore::open("").error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(
        SqliteCredentialStore::open(std::string(kMaxDatabasePathBytes + 1U, 'x')).error().code(),
        common::ErrorCode::invalid_argument);
    EXPECT_EQ(SqliteCredentialStore::open(std::string{"bad\0path", 8U}).error().code(),
              common::ErrorCode::invalid_argument);

    TemporaryDatabaseFile temporary;
    auto store = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(store) << store.error();
    EXPECT_EQ((*store)->path(), temporary.path_string());
    std::string invalid_utf8{"bad"};
    invalid_utf8.push_back(static_cast<char>(0xffU));
    const std::vector<std::string> invalid_keys{"", std::string(kMaxCredentialKeyBytes + 1U, 'x'),
                                                std::string{"a\0b", 3U}, invalid_utf8};
    for (const auto& key : invalid_keys) {
        SCOPED_TRACE(key.size());
        EXPECT_EQ((*store)->put(key, "secret").error().code(), common::ErrorCode::invalid_argument);
        EXPECT_EQ((*store)->get(key).error().code(), common::ErrorCode::invalid_argument);
        EXPECT_EQ((*store)->remove(key).error().code(), common::ErrorCode::invalid_argument);
    }
    EXPECT_EQ(
        (*store)->put("valid", std::string(kMaxCredentialSecretBytes + 1U, 'x')).error().code(),
        common::ErrorCode::invalid_argument);
    EXPECT_EQ((*store)->put("valid", std::string{"a\0b", 3U}).error().code(),
              common::ErrorCode::invalid_argument);
}

TEST(CredentialStoreTest, RejectsUnversionedIncompleteAndMalformedSchemas) {
    const auto expect_schema_error = [](const std::string_view setup,
                                        const common::ErrorCode expected) {
        TemporaryDatabaseFile temporary;
        {
            NativeSqliteDatabase fixture{temporary.path()};
            fixture.execute(setup);
        }
        const auto store = SqliteCredentialStore::open(temporary.path_string());
        ASSERT_FALSE(store);
        EXPECT_EQ(store.error().code(), expected) << store.error();
    };
    expect_schema_error("CREATE TABLE unrelated(value TEXT)", common::ErrorCode::database_error);
    expect_schema_error("PRAGMA user_version = 1; CREATE TABLE unrelated(value TEXT)",
                        common::ErrorCode::database_error);
    expect_schema_error("PRAGMA user_version = 1; CREATE TABLE credentials(key TEXT, secret BLOB)",
                        common::ErrorCode::database_error);
    expect_schema_error(
        "PRAGMA user_version = 1; CREATE TABLE credentials(key TEXT, secret BLOB, updated_at "
        "INTEGER); CREATE TABLE extra(value TEXT)",
        common::ErrorCode::database_error);
}

TEST(CredentialStoreTest, MigrationCreationFailureRollsBackWithoutCredentialsTable) {
    TemporaryDatabaseFile temporary;
    {
        NativeSqliteDatabase fixture{temporary.path()};
        fixture.execute(
            "CREATE VIEW credentials AS SELECT 1 AS key, 2 AS secret, 3 AS updated_at");
    }

    const auto store = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_FALSE(store);
    EXPECT_EQ(store.error().code(), common::ErrorCode::database_error);
    {
        NativeSqliteDatabase probe{temporary.path(),
                                   SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX};
        EXPECT_EQ(
            probe.query_int64("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                              "AND name = 'credentials'"),
            0);
        EXPECT_EQ(probe.query_int64("PRAGMA user_version"), 0);
    }
}

TEST(CredentialStoreTest, ContainsSqlFailuresAndRejectsMalformedStoredSecrets) {
    {
        TemporaryDatabaseFile temporary;
        auto store = SqliteCredentialStore::open(temporary.path_string());
        ASSERT_TRUE(store) << store.error();
        NativeSqliteDatabase injector{temporary.path()};
        injector.execute("CREATE TRIGGER reject_put BEFORE INSERT ON credentials BEGIN "
                         "SELECT RAISE(ABORT, 'injected put failure'); END");
        const auto rejected = (*store)->put("key", "secret");
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code(), common::ErrorCode::invalid_argument);
        injector.execute("DROP TRIGGER reject_put");
        ASSERT_TRUE((*store)->put("key", "secret"));
        injector.execute("CREATE TRIGGER reject_remove BEFORE DELETE ON credentials BEGIN "
                         "SELECT RAISE(ABORT, 'injected remove failure'); END");
        const auto remove = (*store)->remove("key");
        ASSERT_FALSE(remove);
        EXPECT_EQ(remove.error().code(), common::ErrorCode::invalid_argument);
        ASSERT_TRUE((*store)->get("key"));
    }
    {
        TemporaryDatabaseFile temporary;
        auto store = SqliteCredentialStore::open(temporary.path_string());
        ASSERT_TRUE(store) << store.error();
        NativeSqliteDatabase injector{temporary.path()};
        injector.execute("DROP TABLE credentials");
        EXPECT_EQ((*store)->put("key", "secret").error().code(), common::ErrorCode::database_error);
        EXPECT_EQ((*store)->get("key").error().code(), common::ErrorCode::database_error);
        EXPECT_EQ((*store)->remove("key").error().code(), common::ErrorCode::database_error);
    }
    {
        TemporaryDatabaseFile temporary;
        auto store = SqliteCredentialStore::open(temporary.path_string());
        ASSERT_TRUE(store) << store.error();
        NativeSqliteDatabase injector{temporary.path()};
        injector.execute("DROP TABLE credentials; CREATE TABLE credentials(key TEXT, secret, "
                         "updated_at INTEGER)");
        injector.execute("INSERT INTO credentials VALUES('key', 'text-secret', 1)");
        EXPECT_EQ((*store)->get("key").error().code(), common::ErrorCode::database_error);
        injector.execute("DELETE FROM credentials; INSERT INTO credentials VALUES('key', X'', 1)");
        EXPECT_EQ((*store)->get("key").error().code(), common::ErrorCode::database_error);
        injector.execute("DELETE FROM credentials; INSERT INTO credentials VALUES('key', X'61', "
                         "1), ('key', X'62', 2)");
        EXPECT_EQ((*store)->get("key").error().code(), common::ErrorCode::database_error);
    }
}

TEST(CredentialStoreTest, ValidatesBackupsAndRestoresAtomically) {
    TemporaryDatabaseFile live_file;
    TemporaryDatabaseFile source_file;
    auto live = SqliteCredentialStore::open(live_file.path_string());
    auto source = SqliteCredentialStore::open(source_file.path_string());
    ASSERT_TRUE(live) << live.error();
    ASSERT_TRUE(source) << source.error();
    ASSERT_TRUE((*live)->put("old", "old-secret"));
    ASSERT_TRUE((*source)->put("new", "new-secret"));

    ASSERT_TRUE((*live)->validate_restore_source(source_file.path_string()));
    ASSERT_TRUE((*live)->restore_from(source_file.path_string()));
    auto restored = (*live)->get("new");
    ASSERT_TRUE(restored) << restored.error();
    EXPECT_EQ(restored->view(), "new-secret");
    EXPECT_EQ((*live)->get("old").error().code(), common::ErrorCode::not_found);

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

    const auto backup = live_file.directory() / "backup.db";
    ASSERT_TRUE((*live)->backup_to(backup.string()));
    EXPECT_EQ((*live)->backup_to(backup.string()).error().code(),
              common::ErrorCode::already_exists);
    EXPECT_EQ((*live)->validate_restore_source(live_file.path_string()).error().code(),
              common::ErrorCode::invalid_argument);
}

TEST(CredentialStoreTest, DiagnosticsFailsCleanlyWhenTheBackingPathDisappears) {
    TemporaryDatabaseFile temporary;
    auto store = SqliteCredentialStore::open(temporary.path_string());
    ASSERT_TRUE(store) << store.error();
    ASSERT_TRUE(std::filesystem::remove(temporary.path()));
    const auto diagnostics = (*store)->diagnostics();
    ASSERT_FALSE(diagnostics);
    EXPECT_EQ(diagnostics.error().code(), common::ErrorCode::database_error);
}

} // namespace
} // namespace minitun::storage
