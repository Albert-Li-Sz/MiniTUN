#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <sys/stat.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <minitun/common/error.hpp>

#include "sqlite_internal.hpp"
#include "storage_test_support.hpp"

namespace minitun::storage::internal {
namespace {

using test::NativeSqliteDatabase;
using test::TemporaryDatabaseFile;

TEST(SqliteInternalTest, PreparesBindsMovesAndStepsStatements) {
    TemporaryDatabaseFile temporary;
    NativeSqliteDatabase database{temporary.path()};

    auto malformed = Statement::prepare(database.handle(), "not valid SQL", "prepare malformed");
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code(), common::ErrorCode::database_error);

    auto statement = Statement::prepare(database.handle(), "SELECT ?1, ?2, ?3, ?4", "bind values");
    ASSERT_TRUE(statement) << statement.error();
    EXPECT_FALSE(statement->bind_null(99));
    EXPECT_FALSE(statement->bind_text(99, "out of range"));
    EXPECT_FALSE(statement->bind_blob(99, "out of range"));
    EXPECT_FALSE(statement->bind_int64(99, 1));
    ASSERT_TRUE(statement->bind_null(1));
    ASSERT_TRUE(statement->bind_text(2, "text"));
    ASSERT_TRUE(statement->bind_blob(3, "blob"));
    ASSERT_TRUE(statement->bind_int64(4, 42));

    Statement moved{std::move(*statement)};
    auto row = moved.step();
    ASSERT_TRUE(row) << row.error();
    ASSERT_EQ(*row, StepResult::row);
    EXPECT_EQ(moved.handle(), moved.handle());

    EXPECT_FALSE(required_text(moved.handle(), 0, "null text"));
    auto null_text = optional_text(moved.handle(), 0, "optional null text");
    ASSERT_TRUE(null_text) << null_text.error();
    EXPECT_FALSE(null_text->has_value());
    auto text = required_text(moved.handle(), 1, "text");
    ASSERT_TRUE(text) << text.error();
    EXPECT_EQ(*text, "text");
    auto optional = optional_text(moved.handle(), 1, "optional text");
    ASSERT_TRUE(optional) << optional.error();
    EXPECT_EQ(*optional, std::optional<std::string>{"text"});
    EXPECT_FALSE(required_text(moved.handle(), 2, "blob text"));
    EXPECT_FALSE(optional_text(moved.handle(), 2, "optional blob text"));

    EXPECT_FALSE(required_int64(moved.handle(), 0, "null integer"));
    auto null_integer = optional_int64(moved.handle(), 0, "optional null integer");
    ASSERT_TRUE(null_integer) << null_integer.error();
    EXPECT_FALSE(null_integer->has_value());
    auto integer = required_int64(moved.handle(), 3, "integer");
    ASSERT_TRUE(integer) << integer.error();
    EXPECT_EQ(*integer, 42);
    auto optional_integer = optional_int64(moved.handle(), 3, "optional integer");
    ASSERT_TRUE(optional_integer) << optional_integer.error();
    EXPECT_EQ(*optional_integer, std::optional<std::int64_t>{42});
    EXPECT_FALSE(optional_int64(moved.handle(), 1, "optional text integer"));

    auto done = moved.step();
    ASSERT_TRUE(done) << done.error();
    EXPECT_EQ(*done, StepResult::done);
}

TEST(SqliteInternalTest, MapsEveryStableSqliteErrorClassAndExecResult) {
    struct Mapping final {
        int sqlite_code;
        common::ErrorCode expected;
    };
    constexpr std::array mappings{
        Mapping{SQLITE_CONSTRAINT_PRIMARYKEY, common::ErrorCode::already_exists},
        Mapping{SQLITE_CONSTRAINT_UNIQUE, common::ErrorCode::already_exists},
        Mapping{SQLITE_CONSTRAINT_FOREIGNKEY, common::ErrorCode::not_found},
        Mapping{SQLITE_CONSTRAINT_CHECK, common::ErrorCode::invalid_argument},
        Mapping{SQLITE_NOMEM, common::ErrorCode::resource_exhausted},
        Mapping{SQLITE_FULL, common::ErrorCode::resource_exhausted},
        Mapping{SQLITE_TOOBIG, common::ErrorCode::resource_exhausted},
        Mapping{SQLITE_PERM, common::ErrorCode::permission_denied},
        Mapping{SQLITE_AUTH, common::ErrorCode::permission_denied},
        Mapping{SQLITE_READONLY, common::ErrorCode::permission_denied},
        Mapping{SQLITE_BUSY, common::ErrorCode::database_error},
    };
    for (const auto& mapping : mappings) {
        SCOPED_TRACE(mapping.sqlite_code);
        const auto error = sqlite_error(nullptr, mapping.sqlite_code, "mapped operation");
        EXPECT_EQ(error.code(), mapping.expected);
        EXPECT_EQ(error.message(), "mapped operation");
    }

    TemporaryDatabaseFile temporary;
    NativeSqliteDatabase database{temporary.path()};
    EXPECT_TRUE(execute(database.handle(), "CREATE TABLE values_table(value INTEGER UNIQUE)",
                        "create values"));
    EXPECT_TRUE(execute(database.handle(), "INSERT INTO values_table VALUES(1)", "insert value"));
    const auto duplicate =
        execute(database.handle(), "INSERT INTO values_table VALUES(1)", "duplicate value");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), common::ErrorCode::already_exists);
    EXPECT_NE(duplicate.error().message().find("UNIQUE"), std::string::npos);

    auto insert = Statement::prepare(database.handle(), "INSERT INTO values_table VALUES(?1)",
                                     "step duplicate");
    ASSERT_TRUE(insert) << insert.error();
    ASSERT_TRUE(insert->bind_int64(1, 1));
    const auto step = insert->step();
    ASSERT_FALSE(step);
    EXPECT_EQ(step.error().code(), common::ErrorCode::already_exists);
}

TEST(SqliteInternalTest, QueriesExactlyOneTypedRow) {
    TemporaryDatabaseFile temporary;
    NativeSqliteDatabase database{temporary.path()};

    auto integer = query_single_int64(database.handle(), "SELECT 42", "integer");
    ASSERT_TRUE(integer) << integer.error();
    EXPECT_EQ(*integer, 42);
    EXPECT_FALSE(query_single_int64(database.handle(), "SELECT 1 WHERE 0", "missing integer"));
    EXPECT_FALSE(query_single_int64(database.handle(), "SELECT 'text'", "wrong integer type"));
    EXPECT_FALSE(
        query_single_int64(database.handle(), "SELECT 1 UNION ALL SELECT 2", "extra integer"));
    EXPECT_FALSE(query_single_int64(database.handle(), "not SQL", "malformed integer"));

    auto text = query_single_text(database.handle(), "SELECT 'value'", "text");
    ASSERT_TRUE(text) << text.error();
    EXPECT_EQ(*text, "value");
    EXPECT_FALSE(query_single_text(database.handle(), "SELECT 'value' WHERE 0", "missing text"));
    EXPECT_FALSE(query_single_text(database.handle(), "SELECT 7", "wrong text type"));
    EXPECT_FALSE(
        query_single_text(database.handle(), "SELECT 'a' UNION ALL SELECT 'b'", "extra text"));
    EXPECT_FALSE(query_single_text(database.handle(), "not SQL", "malformed text"));
}

TEST(SqliteInternalTest, ValidatesCanonicalUtf8AndEveryMalformedBoundaryClass) {
    constexpr std::array<std::string_view, 13> valid{
        "",
        "ascii",
        "\xc2\xa2",
        "\xdf\xbf",
        "\xe0\xa0\x80",
        "\xed\x9f\xbf",
        "\xe1\x80\x80",
        "\xec\xbf\xbf",
        "\xee\x80\x80",
        "\xef\xbf\xbf",
        "\xf0\x90\x80\x80",
        "\xf4\x8f\xbf\xbf",
        "\xf2\x80\x80\x80",
    };
    for (const auto value : valid) {
        EXPECT_TRUE(validate_text(value, 0U, 16U, "valid UTF-8")) << value.size();
    }

    constexpr std::array<std::string_view, 15> invalid{
        "\x80",
        "\xc1\x80",
        "\xc2",
        "\xc2"
        "A",
        "\xe0\x9f\x80",
        "\xe0\xa0",
        "\xed\xa0\x80",
        "\xe1\x80"
        "A",
        "\xf0\x8f\x80\x80",
        "\xf4\x90\x80\x80",
        "\xf1\x80\x80"
        "A",
        "\xf0\x90"
        "A\x80",
        "\xf0\x90\x80"
        "A",
        "\xf0\x90\x80",
        "\xf5\x80\x80\x80",
    };
    for (const auto value : invalid) {
        const auto result = validate_text(value, 0U, 16U, "invalid UTF-8");
        ASSERT_FALSE(result) << value.size();
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    }
    EXPECT_FALSE(validate_text("x", 2U, 3U, "short"));
    EXPECT_FALSE(validate_text("long", 0U, 3U, "long"));
    EXPECT_FALSE(validate_text(std::string_view{"a\0b", 3U}, 0U, 3U, "NUL"));
}

TEST(SqliteInternalTest, RejectsUnsafeBackupDestinationsBeforeCopying) {
    TemporaryDatabaseFile temporary;
    NativeSqliteDatabase database{temporary.path()};
    const std::string source = temporary.path_string();
    const std::string oversized(4'097U, 'x');
    const std::string nul_path{"bad\0path", 8U};

    EXPECT_FALSE(backup_database(nullptr, source, "backup.db", "backup"));
    EXPECT_FALSE(backup_database(database.handle(), source, "", "backup"));
    EXPECT_FALSE(backup_database(database.handle(), source, oversized, "backup"));
    EXPECT_FALSE(backup_database(database.handle(), source, nul_path, "backup"));
    EXPECT_FALSE(backup_database(database.handle(), source, source, "backup"));
    EXPECT_FALSE(backup_database(database.handle(), source,
                                 (temporary.directory() / "missing" / "backup.db").string(),
                                 "backup"));

    const auto existing = temporary.directory() / "existing.db";
    test::write_binary_file(existing, "existing");
    EXPECT_FALSE(backup_database(database.handle(), source, existing.string(), "backup"));

    const auto unsafe_directory = temporary.directory() / "unsafe";
    ASSERT_TRUE(std::filesystem::create_directory(unsafe_directory));
    ASSERT_EQ(::chmod(unsafe_directory.c_str(), 0777), 0);
    const auto unsafe = backup_database(database.handle(), source,
                                        (unsafe_directory / "backup.db").string(), "backup");
    ASSERT_FALSE(unsafe);
    EXPECT_EQ(unsafe.error().code(), common::ErrorCode::permission_denied);
}

TEST(SqliteInternalTest, ValidatesRestorePathsPermissionsIntegrityAndCallbacks) {
    TemporaryDatabaseFile temporary;
    const auto destination = (temporary.directory() / "destination.db").string();
    NativeSqliteDatabase source{temporary.path()};
    ASSERT_EQ(::chmod(temporary.path().c_str(), 0600), 0);
    const std::string oversized(4'097U, 'x');
    const std::string nul_path{"bad\0path", 8U};

    EXPECT_FALSE(validate_restore_source(destination, "", "restore"));
    EXPECT_FALSE(validate_restore_source(destination, oversized, "restore"));
    EXPECT_FALSE(validate_restore_source(destination, nul_path, "restore"));
    EXPECT_FALSE(validate_restore_source("", temporary.path_string(), "restore"));
    EXPECT_FALSE(validate_restore_source(oversized, temporary.path_string(), "restore"));
    EXPECT_FALSE(validate_restore_source(nul_path, temporary.path_string(), "restore"));
    EXPECT_FALSE(
        validate_restore_source(temporary.path_string(), temporary.path_string(), "restore"));
    EXPECT_FALSE(validate_restore_source(
        destination, (temporary.directory() / "missing" / "source.db").string(), "restore"));
    EXPECT_FALSE(validate_restore_source(destination, temporary.directory().string(), "restore"));

    ASSERT_EQ(::chmod(temporary.path().c_str(), 0644), 0);
    const auto unsafe = validate_restore_source(destination, temporary.path_string(), "restore");
    ASSERT_FALSE(unsafe);
    EXPECT_EQ(unsafe.error().code(), common::ErrorCode::permission_denied);
    ASSERT_EQ(::chmod(temporary.path().c_str(), 0600), 0);

    const auto invalid_database = temporary.directory() / "invalid.db";
    test::write_binary_file(invalid_database, "not a SQLite database");
    ASSERT_EQ(::chmod(invalid_database.c_str(), 0600), 0);
    EXPECT_FALSE(validate_restore_source(destination, invalid_database.string(), "restore"));

    EXPECT_FALSE(
        validate_restore_source(destination, temporary.path_string(), "restore", [](sqlite3*) {
            return common::Result<void>::failure(common::ErrorCode::unsupported_version,
                                                 "rejected by schema validator");
        }));
    EXPECT_TRUE(validate_restore_source(destination, temporary.path_string(), "restore"));
    EXPECT_TRUE(validate_restore_source(destination, temporary.path_string(), "restore",
                                        [](sqlite3*) { return common::Result<void>::success(); }));
    EXPECT_FALSE(restore_database(nullptr, destination, temporary.path_string(), "restore"));
}

TEST(SqliteInternalTest, FormatsEndpointTextWithoutLosingHostOrPort) {
    EXPECT_EQ(endpoint_text("127.0.0.1", 65'535U), "127.0.0.1:65535");
    EXPECT_EQ(endpoint_text("::1", 443U), "[::1]:443");
}

} // namespace
} // namespace minitun::storage::internal
