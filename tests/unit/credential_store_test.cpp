#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/storage/credential_store.hpp>

#include "storage_test_support.hpp"

namespace minitun::storage {
namespace {

using test::NativeSqliteDatabase;
using test::TemporaryDatabaseFile;

TEST(CredentialStoreTest, PersistsUpdatesAndRemovesSecretsInPrivateFile) {
    TemporaryDatabaseFile temporary;
    auto store = SqliteCredentialStore::open(temporary.path_string());

    ASSERT_TRUE(store) << store.error();
    struct stat status {};
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

} // namespace
} // namespace minitun::storage
