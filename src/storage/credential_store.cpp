#include <minitun/storage/credential_store.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <minitun/common/error.hpp>
#include <minitun/common/time.hpp>
#include <minitun/storage/database.hpp>

#include "sqlite_internal.hpp"

namespace minitun::storage {
namespace {

constexpr int kCredentialSchemaVersion = 1;
constexpr std::string_view kCreateCredentials = R"sql(
CREATE TABLE credentials (
    key TEXT NOT NULL PRIMARY KEY
        CHECK(
            typeof(key) = 'text'
            AND length(CAST(key AS BLOB)) BETWEEN 1 AND 256
        ),
    secret BLOB NOT NULL
        CHECK(
            typeof(secret) = 'blob'
            AND length(secret) BETWEEN 1 AND 65536
        ),
    updated_at INTEGER NOT NULL
        CHECK(typeof(updated_at) = 'integer' AND updated_at >= 0)
)
)sql";

[[nodiscard]] common::Error posix_error(const int error_number,
                                         const std::string_view operation) {
    common::ErrorCode code = common::ErrorCode::database_error;
    if (error_number == EACCES || error_number == EPERM || error_number == ELOOP) {
        code = common::ErrorCode::permission_denied;
    } else if (error_number == ENOENT || error_number == ENOTDIR) {
        code = common::ErrorCode::not_found;
    }
    std::string message{operation};
    message.append(" failed: ");
    message.append(std::strerror(error_number));
    return common::Error{code, std::move(message)};
}

[[nodiscard]] common::Result<void> validate_key(const std::string_view key) {
    return internal::validate_text(key, 1U, kMaxCredentialKeyBytes, "credential key");
}

[[nodiscard]] common::Result<void> validate_secret(const std::string_view secret) {
    if (secret.empty() || secret.size() > kMaxCredentialSecretBytes) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "credential secret is outside its byte-length limit");
    }
    if (secret.find('\0') != std::string_view::npos) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "credential secret must not contain NUL bytes");
    }
    return common::Result<void>::success();
}

[[nodiscard]] std::string parent_path(const std::string_view path) {
    const auto slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return ".";
    }
    if (slash == 0U) {
        return "/";
    }
    return std::string{path.substr(0U, slash)};
}

[[nodiscard]] common::Result<void> prepare_credential_file(const std::string_view path) {
    const std::string parent = parent_path(path);
    struct stat parent_status {};
    if (::lstat(parent.c_str(), &parent_status) != 0) {
        return posix_error(errno, "credential database directory inspection");
    }
    if (!S_ISDIR(parent_status.st_mode) || parent_status.st_uid != ::geteuid() ||
        (parent_status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return common::Result<void>::failure(
            common::ErrorCode::permission_denied,
            "credential database directory must be private and owned by the daemon user");
    }

    const std::string owned_path{path};
    const int descriptor = ::open(owned_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                  S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return posix_error(errno, "credential database file open");
    }

    common::Result<void> result = common::Result<void>::success();
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        result = posix_error(errno, "credential database file inspection");
    } else if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1) {
        result = common::Result<void>::failure(
            common::ErrorCode::permission_denied,
            "credential database must be a daemon-owned regular file with one link");
    } else if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        result = posix_error(errno, "credential database permission update");
    }

    const int close_result = ::close(descriptor);
    if (result && close_result != 0) {
        return posix_error(errno, "credential database file close");
    }
    return result;
}

[[nodiscard]] common::Result<void> rollback_with_error(sqlite3* database, common::Error error) {
    auto rolled_back = internal::execute(database, "ROLLBACK", "roll back credential transaction");
    if (!rolled_back) {
        return common::Result<void>::failure(
            common::ErrorCode::database_error,
            "credential operation failed and its transaction could not be rolled back");
    }
    return error;
}

} // namespace

SqliteCredentialStore::SqliteCredentialStore(sqlite3* handle, std::string path) noexcept
    : handle_(handle), path_(std::move(path)) {}

SqliteCredentialStore::~SqliteCredentialStore() noexcept {
    std::scoped_lock lock{mutex_};
    if (handle_ != nullptr) {
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

common::Result<std::unique_ptr<SqliteCredentialStore>>
SqliteCredentialStore::open(const std::string_view path) {
    if (path.empty() || path.size() > kMaxDatabasePathBytes ||
        path.find('\0') != std::string_view::npos) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "credential database path is empty, oversized, or contains NUL"};
    }
    auto prepared = prepare_credential_file(path);
    if (!prepared) {
        return prepared.error();
    }

    sqlite3* handle = nullptr;
    std::string owned_path{path};
    const int result = sqlite3_open_v2(
        owned_path.c_str(), &handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK) {
        common::Error error = internal::sqlite_error(handle, result, "open credential database");
        if (handle != nullptr) {
            sqlite3_close_v2(handle);
        }
        return error;
    }

    sqlite3_extended_result_codes(handle, 1);
    auto store = std::unique_ptr<SqliteCredentialStore>{
        new SqliteCredentialStore{handle, std::move(owned_path)}};
    if (auto configured = store->configure(); !configured) {
        return configured.error();
    }
    if (auto migrated = store->migrate(); !migrated) {
        return migrated.error();
    }
    return store;
}

common::Result<void> SqliteCredentialStore::configure() {
    std::scoped_lock lock{mutex_};
    if (sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") == 0) {
        int enabled = 0;
        const int result =
            sqlite3_db_config(handle_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, &enabled);
        if (result != SQLITE_OK || enabled != 0) {
            return common::Result<void>::failure(common::ErrorCode::database_error,
                                                 "failed to disable credential DB extensions");
        }
    }
    if (sqlite3_busy_timeout(handle_, kDatabaseBusyTimeoutMilliseconds) != SQLITE_OK) {
        return common::Result<void>::failure(common::ErrorCode::database_error,
                                             "failed to configure credential DB timeout");
    }
    constexpr std::pair<std::string_view, std::string_view> pragmas[]{
        {"PRAGMA journal_mode = DELETE", "configure credential journal mode"},
        {"PRAGMA synchronous = FULL", "configure credential durability"},
        {"PRAGMA secure_delete = ON", "enable secure credential deletion"},
        {"PRAGMA temp_store = MEMORY", "keep credential temporary data in memory"},
    };
    for (const auto& [sql, operation] : pragmas) {
        if (auto configured = internal::execute(handle_, sql, operation); !configured) {
            return configured;
        }
    }
    return common::Result<void>::success();
}

common::Result<void> SqliteCredentialStore::migrate() {
    std::scoped_lock lock{mutex_};
    auto version = internal::query_single_int64(handle_, "PRAGMA user_version",
                                                "read credential schema version");
    if (!version) {
        return version.error();
    }
    if (*version > kCredentialSchemaVersion) {
        return common::Result<void>::failure(common::ErrorCode::unsupported_version,
                                             "credential database schema is newer than this build");
    }

    auto table_count = internal::query_single_int64(
        handle_, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name NOT GLOB 'sqlite_*'",
        "inspect credential schema");
    if (!table_count) {
        return table_count.error();
    }

    if (*version == 0) {
        if (*table_count != 0) {
            return common::Result<void>::failure(
                common::ErrorCode::database_error,
                "refusing to migrate an unversioned non-empty credential database");
        }
        auto begun = internal::execute(handle_, "BEGIN IMMEDIATE", "begin credential migration");
        if (!begun) {
            return begun;
        }
        auto created = internal::execute(handle_, kCreateCredentials, "create credentials table");
        if (!created) {
            return rollback_with_error(handle_, created.error());
        }
        auto recorded = internal::execute(handle_, "PRAGMA user_version = 1",
                                          "record credential schema version");
        if (!recorded) {
            return rollback_with_error(handle_, recorded.error());
        }
        auto committed = internal::execute(handle_, "COMMIT", "commit credential migration");
        if (!committed) {
            return rollback_with_error(handle_, committed.error());
        }
    } else if (*table_count != 1) {
        return common::Result<void>::failure(common::ErrorCode::database_error,
                                             "credential database schema is incomplete");
    }

    auto columns = internal::query_single_int64(
        handle_, "SELECT COUNT(*) FROM pragma_table_info('credentials')",
        "validate credential schema");
    if (!columns || *columns != 3) {
        return common::Result<void>::failure(common::ErrorCode::database_error,
                                             "credential database schema is invalid");
    }
    auto integrity = internal::query_single_text(handle_, "PRAGMA integrity_check(1)",
                                                 "validate credential database integrity");
    if (!integrity || *integrity != "ok") {
        return common::Result<void>::failure(common::ErrorCode::database_error,
                                             "credential database failed its integrity check");
    }
    return common::Result<void>::success();
}

common::Result<void> SqliteCredentialStore::put(const std::string_view key,
                                                 const std::string_view secret) {
    if (auto valid = validate_key(key); !valid) {
        return valid;
    }
    if (auto valid = validate_secret(secret); !valid) {
        return valid;
    }

    std::scoped_lock lock{mutex_};
    auto begun = internal::execute(handle_, "BEGIN IMMEDIATE", "begin credential update");
    if (!begun) {
        return begun;
    }
    auto statement = internal::Statement::prepare(
        handle_,
        "INSERT INTO credentials(key, secret, updated_at) VALUES(?1, ?2, ?3) "
        "ON CONFLICT(key) DO UPDATE SET secret = excluded.secret, updated_at = excluded.updated_at",
        "store credential");
    if (!statement) {
        return rollback_with_error(handle_, statement.error());
    }
    if (auto bound = statement->bind_text(1, key); !bound) {
        return rollback_with_error(handle_, bound.error());
    }
    if (auto bound = statement->bind_blob(2, secret); !bound) {
        return rollback_with_error(handle_, bound.error());
    }
    if (auto bound = statement->bind_int64(3, common::unix_milliseconds_now()); !bound) {
        return rollback_with_error(handle_, bound.error());
    }
    auto step = statement->step();
    if (!step || *step != internal::StepResult::done) {
        return rollback_with_error(
            handle_, !step ? step.error()
                           : common::Error{common::ErrorCode::database_error,
                                           "credential update unexpectedly returned a row"});
    }
    auto committed = internal::execute(handle_, "COMMIT", "commit credential update");
    if (!committed) {
        return rollback_with_error(handle_, committed.error());
    }
    return common::Result<void>::success();
}

common::Result<common::SecureString>
SqliteCredentialStore::get(const std::string_view key) const {
    if (auto valid = validate_key(key); !valid) {
        return valid.error();
    }
    std::scoped_lock lock{mutex_};
    auto statement = internal::Statement::prepare(handle_,
                                                  "SELECT secret FROM credentials WHERE key = ?1",
                                                  "read credential");
    if (!statement) {
        return statement.error();
    }
    if (auto bound = statement->bind_text(1, key); !bound) {
        return bound.error();
    }
    auto step = statement->step();
    if (!step) {
        return step.error();
    }
    if (*step == internal::StepResult::done) {
        return common::Error{common::ErrorCode::not_found, "credential was not found"};
    }
    if (sqlite3_column_type(statement->handle(), 0) != SQLITE_BLOB) {
        return common::Error{common::ErrorCode::database_error,
                             "credential database contains an invalid secret"};
    }
    const int byte_count = sqlite3_column_bytes(statement->handle(), 0);
    const void* const data = sqlite3_column_blob(statement->handle(), 0);
    if (byte_count < 1 || static_cast<std::size_t>(byte_count) > kMaxCredentialSecretBytes ||
        data == nullptr) {
        return common::Error{common::ErrorCode::database_error,
                             "credential database contains an invalid secret"};
    }
    try {
        common::SecureString secret{std::string_view{static_cast<const char*>(data),
                                                     static_cast<std::size_t>(byte_count)}};
        step = statement->step();
        if (!step || *step != internal::StepResult::done) {
            return !step ? step.error()
                         : common::Error{common::ErrorCode::database_error,
                                         "credential lookup returned duplicate rows"};
        }
        return secret;
    } catch (const std::bad_alloc&) {
        return common::Error{common::ErrorCode::resource_exhausted,
                             "insufficient memory while reading credential"};
    }
}

common::Result<void> SqliteCredentialStore::remove(const std::string_view key) {
    if (auto valid = validate_key(key); !valid) {
        return valid;
    }
    std::scoped_lock lock{mutex_};
    auto begun = internal::execute(handle_, "BEGIN IMMEDIATE", "begin credential removal");
    if (!begun) {
        return begun;
    }
    auto statement = internal::Statement::prepare(
        handle_, "DELETE FROM credentials WHERE key = ?1", "remove credential");
    if (!statement) {
        return rollback_with_error(handle_, statement.error());
    }
    if (auto bound = statement->bind_text(1, key); !bound) {
        return rollback_with_error(handle_, bound.error());
    }
    auto step = statement->step();
    if (!step || *step != internal::StepResult::done) {
        return rollback_with_error(
            handle_, !step ? step.error()
                           : common::Error{common::ErrorCode::database_error,
                                           "credential removal unexpectedly returned a row"});
    }
    auto committed = internal::execute(handle_, "COMMIT", "commit credential removal");
    if (!committed) {
        return rollback_with_error(handle_, committed.error());
    }
    return common::Result<void>::success();
}

const std::string& SqliteCredentialStore::path() const noexcept { return path_; }

} // namespace minitun::storage
