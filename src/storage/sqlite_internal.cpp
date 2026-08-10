#include "sqlite_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>

namespace minitun::storage::internal {
namespace {

constexpr std::size_t kMaximumDatabasePathBytes = 4'096U;

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

[[nodiscard]] common::Result<void> validate_private_parent(const std::string_view path,
                                                           const std::string_view description) {
    const std::string parent = parent_path(path);
    struct stat status{};
    if (::lstat(parent.c_str(), &status) != 0) {
        return common::Error{common::ErrorCode::not_found,
                             std::string{description} + " parent directory is unavailable"};
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return common::Error{common::ErrorCode::permission_denied,
                             std::string{description} +
                                 " parent directory must be daemon-owned and private"};
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> fsync_file(const int descriptor,
                                              const std::string_view description) {
    if (::fsync(descriptor) == 0) {
        return common::Result<void>::success();
    }
    return common::Error{common::ErrorCode::database_error,
                         std::string{description} + " fsync failed"};
}

[[nodiscard]] common::Result<void> fsync_directory(const std::string_view path,
                                                   const std::string_view description) {
    const std::string parent = parent_path(path);
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{description} + " directory open failed"};
    }
    const int result = ::fsync(descriptor);
    const int saved_errno = errno;
    static_cast<void>(::close(descriptor));
    if (result == 0) {
        return common::Result<void>::success();
    }
    static_cast<void>(saved_errno);
    return common::Error{common::ErrorCode::database_error,
                         std::string{description} + " directory fsync failed"};
}

[[nodiscard]] common::Result<void> remove_temporary(const std::string& path, common::Error error) {
    static_cast<void>(::unlink(path.c_str()));
    return error;
}

[[nodiscard]] common::Error corrupt_field(const std::string_view field) {
    return common::Error{common::ErrorCode::database_error,
                         std::string{"database contains an invalid "} + std::string{field}};
}

[[nodiscard]] bool is_utf8_continuation(const unsigned char value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

[[nodiscard]] bool is_valid_utf8(const std::string_view value) noexcept {
    std::size_t offset = 0U;
    while (offset < value.size()) {
        const auto byte = [&value, offset](const std::size_t relative_offset) {
            return static_cast<unsigned char>(value[offset + relative_offset]);
        };

        const unsigned char first = byte(0);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }

        if (first >= 0xc2U && first <= 0xdfU) {
            if (offset + 1U >= value.size() || !is_utf8_continuation(byte(1))) {
                return false;
            }
            offset += 2U;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU) {
            if (offset + 2U >= value.size() || !is_utf8_continuation(byte(2))) {
                return false;
            }
            const unsigned char second = byte(1);
            const bool valid_second =
                (first == 0xe0U && second >= 0xa0U && second <= 0xbfU) ||
                (first == 0xedU && second >= 0x80U && second <= 0x9fU) ||
                (((first >= 0xe1U && first <= 0xecU) || (first >= 0xeeU && first <= 0xefU)) &&
                 is_utf8_continuation(second));
            if (!valid_second) {
                return false;
            }
            offset += 3U;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U) {
            if (offset + 3U >= value.size() || !is_utf8_continuation(byte(2)) ||
                !is_utf8_continuation(byte(3))) {
                return false;
            }
            const unsigned char second = byte(1);
            const bool valid_second =
                (first == 0xf0U && second >= 0x90U && second <= 0xbfU) ||
                (first == 0xf4U && second >= 0x80U && second <= 0x8fU) ||
                (first >= 0xf1U && first <= 0xf3U && is_utf8_continuation(second));
            if (!valid_second) {
                return false;
            }
            offset += 4U;
            continue;
        }

        return false;
    }

    return true;
}

[[nodiscard]] bool is_known_error_code(const common::ErrorCode code) noexcept {
    if (code == common::ErrorCode::ok) {
        return false;
    }
    const auto parsed = common::error_code_from_string(common::to_string(code));
    return parsed.has_value() && *parsed == code;
}

template <typename Enum, typename Parser>
[[nodiscard]] bool is_known_enum(const Enum value, Parser parser) noexcept {
    const auto parsed = parser(to_string(value));
    return parsed.has_value() && *parsed == value;
}

[[nodiscard]] common::Result<void> validate_optional_text(const std::optional<std::string>& value,
                                                          const std::size_t minimum_bytes,
                                                          const std::size_t maximum_bytes,
                                                          const std::string_view field) {
    if (!value.has_value()) {
        return common::Result<void>::success();
    }
    return validate_text(*value, minimum_bytes, maximum_bytes, field);
}

[[nodiscard]] common::Result<void>
validate_error_fields(const std::optional<common::ErrorCode> error_code,
                      const std::optional<std::string>& error_message) {
    if (error_code.has_value() && !is_known_error_code(*error_code)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "last_error_code must be a known non-ok error code"};
    }
    return validate_optional_text(error_message, 0U, kMaxErrorMessageBytes, "last_error_message");
}

[[nodiscard]] common::Result<common::Endpoint> read_endpoint(const std::string_view text,
                                                             const std::string_view field) {
    auto parsed = common::Endpoint::parse(text);
    if (!parsed || parsed->to_string() != text) {
        return corrupt_field(field);
    }
    return parsed;
}

[[nodiscard]] common::Result<common::Endpoint> read_split_endpoint(const std::string_view host,
                                                                   const std::int64_t port,
                                                                   const std::string_view field) {
    if (port < 1 || port > 65'535) {
        return corrupt_field(field);
    }
    auto parsed = common::Endpoint::parse(endpoint_text(host, static_cast<std::uint16_t>(port)));
    if (!parsed || parsed->host() != host || parsed->port() != static_cast<std::uint16_t>(port)) {
        return corrupt_field(field);
    }
    return parsed;
}

} // namespace

Statement::Statement(sqlite3* database, sqlite3_stmt* statement, std::string operation)
    : database_(database), statement_(statement), operation_(std::move(operation)) {}

common::Result<Statement> Statement::prepare(sqlite3* database, const std::string_view sql,
                                             const std::string_view operation) {
    if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return common::Error{common::ErrorCode::resource_exhausted,
                             "SQLite statement exceeds the platform length limit"};
    }

    sqlite3_stmt* statement = nullptr;
    const int result =
        sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    if (result != SQLITE_OK) {
        return sqlite_error(database, result, operation);
    }
    return Statement{database, statement, std::string{operation}};
}

Statement::~Statement() noexcept {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

Statement::Statement(Statement&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      statement_(std::exchange(other.statement_, nullptr)),
      operation_(std::move(other.operation_)) {}

common::Result<void> Statement::bind_result(const int result) {
    if (result == SQLITE_OK) {
        return common::Result<void>::success();
    }
    return sqlite_error(database_, result, operation_);
}

common::Result<void> Statement::bind_null(const int index) {
    return bind_result(sqlite3_bind_null(statement_, index));
}

common::Result<void> Statement::bind_text(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return common::Error{common::ErrorCode::resource_exhausted,
                             "SQLite text value exceeds the platform length limit"};
    }
    return bind_result(sqlite3_bind_text(statement_, index, value.data(),
                                         static_cast<int>(value.size()), SQLITE_TRANSIENT));
}

common::Result<void> Statement::bind_blob(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return common::Error{common::ErrorCode::resource_exhausted,
                             "SQLite blob value exceeds the platform length limit"};
    }
    return bind_result(sqlite3_bind_blob(statement_, index, value.data(),
                                         static_cast<int>(value.size()), SQLITE_TRANSIENT));
}

common::Result<void> Statement::bind_int64(const int index, const std::int64_t value) {
    return bind_result(sqlite3_bind_int64(statement_, index, value));
}

common::Result<StepResult> Statement::step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
        return StepResult::row;
    }
    if (result == SQLITE_DONE) {
        return StepResult::done;
    }
    return sqlite_error(database_, result, operation_);
}

sqlite3_stmt* Statement::handle() const noexcept { return statement_; }

common::Error sqlite_error(sqlite3* database, const int result, const std::string_view operation) {
    const int extended_result = database == nullptr ? result : sqlite3_extended_errcode(database);
    const int primary_result = extended_result & 0xff;

    common::ErrorCode code = common::ErrorCode::database_error;
    if (extended_result == SQLITE_CONSTRAINT_PRIMARYKEY ||
        extended_result == SQLITE_CONSTRAINT_UNIQUE) {
        code = common::ErrorCode::already_exists;
    } else if (extended_result == SQLITE_CONSTRAINT_FOREIGNKEY) {
        code = common::ErrorCode::not_found;
    } else if (primary_result == SQLITE_CONSTRAINT) {
        code = common::ErrorCode::invalid_argument;
    } else if (primary_result == SQLITE_NOMEM || primary_result == SQLITE_FULL ||
               primary_result == SQLITE_TOOBIG) {
        code = common::ErrorCode::resource_exhausted;
    } else if (primary_result == SQLITE_PERM || primary_result == SQLITE_AUTH ||
               primary_result == SQLITE_READONLY) {
        code = common::ErrorCode::permission_denied;
    }

    std::string message{operation};
    const char* const detail = database == nullptr ? nullptr : sqlite3_errmsg(database);
    if (detail != nullptr && detail[0] != '\0') {
        message.append(": ");
        message.append(detail);
    }
    return common::Error{code, std::move(message)};
}

common::Result<void> execute(sqlite3* database, const std::string_view sql,
                             const std::string_view operation) {
    char* raw_error = nullptr;
    const int result =
        sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &raw_error);
    if (result == SQLITE_OK) {
        return common::Result<void>::success();
    }

    common::Error error = sqlite_error(database, result, operation);
    if (raw_error != nullptr) {
        std::string message{operation};
        message.append(": ");
        message.append(raw_error);
        error = common::Error{error.code(), std::move(message)};
    }
    sqlite3_free(raw_error);
    return error;
}

common::Result<void> backup_database(sqlite3* source_database, const std::string_view source_path,
                                     const std::string_view destination,
                                     const std::string_view description) {
    if (source_database == nullptr || destination.empty() ||
        destination.size() > kMaximumDatabasePathBytes ||
        destination.find('\0') != std::string_view::npos) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{description} + " destination path is invalid"};
    }
    if (destination == source_path) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{description} + " destination must differ from source"};
    }
    if (auto parent = validate_private_parent(destination, description); !parent) {
        return parent;
    }

    struct stat destination_status{};
    if (::lstat(std::string{destination}.c_str(), &destination_status) == 0) {
        return common::Error{common::ErrorCode::already_exists,
                             std::string{description} + " destination already exists"};
    }
    if (errno != ENOENT) {
        return common::Error{common::ErrorCode::permission_denied,
                             std::string{description} + " destination cannot be inspected"};
    }

    std::string temporary_template = parent_path(destination);
    if (temporary_template.back() != '/') {
        temporary_template.push_back('/');
    }
    temporary_template.append(".minitun-backup-XXXXXX");
    std::vector<char> writable_template{temporary_template.begin(), temporary_template.end()};
    writable_template.push_back('\0');
    const int temporary_descriptor = ::mkstemp(writable_template.data());
    if (temporary_descriptor < 0) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{description} + " temporary file creation failed"};
    }
    const std::string temporary_path{writable_template.data()};
    if (::fchmod(temporary_descriptor, S_IRUSR | S_IWUSR) != 0) {
        const int saved_errno = errno;
        static_cast<void>(::close(temporary_descriptor));
        static_cast<void>(::unlink(temporary_path.c_str()));
        static_cast<void>(saved_errno);
        return common::Error{common::ErrorCode::permission_denied,
                             std::string{description} + " temporary permission setup failed"};
    }
    static_cast<void>(::close(temporary_descriptor));

    sqlite3* destination_database = nullptr;
    const int open_result = sqlite3_open_v2(
        temporary_path.c_str(), &destination_database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (open_result != SQLITE_OK) {
        common::Error error = sqlite_error(destination_database, open_result,
                                           std::string{description} + " destination open");
        if (destination_database != nullptr) {
            sqlite3_close_v2(destination_database);
        }
        return remove_temporary(temporary_path, std::move(error));
    }
    sqlite3_extended_result_codes(destination_database, 1);

    sqlite3_backup* backup =
        sqlite3_backup_init(destination_database, "main", source_database, "main");
    if (backup == nullptr) {
        common::Error error =
            sqlite_error(destination_database, sqlite3_errcode(destination_database),
                         std::string{description} + " initialize online backup");
        sqlite3_close_v2(destination_database);
        return remove_temporary(temporary_path, std::move(error));
    }

    int step_result = SQLITE_OK;
    constexpr int kPagesPerStep = 256;
    constexpr int kMaximumBusySteps = 200; // 5 seconds at 25 ms per retry.
    int busy_steps = 0;
    while (true) {
        step_result = sqlite3_backup_step(backup, kPagesPerStep);
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result == SQLITE_OK) {
            busy_steps = 0;
            continue;
        }
        if (step_result == SQLITE_BUSY || step_result == SQLITE_LOCKED) {
            if (++busy_steps > kMaximumBusySteps) {
                break;
            }
            sqlite3_sleep(25);
            continue;
        }
        break;
    }
    const int finish_result = sqlite3_backup_finish(backup);
    if (step_result != SQLITE_DONE || finish_result != SQLITE_OK) {
        const int error_code = finish_result != SQLITE_OK ? finish_result : step_result;
        common::Error error = sqlite_error(destination_database, error_code,
                                           std::string{description} + " copy database pages");
        sqlite3_close_v2(destination_database);
        return remove_temporary(temporary_path, std::move(error));
    }
    // Keep backups readable without write access to the containing directory.
    // A WAL-mode backup would otherwise require a sidecar -shm file when a
    // read-only diagnostic process opens it.
    auto journal_reset = execute(destination_database, "PRAGMA journal_mode = DELETE",
                                 std::string{description} + " normalize backup journal");
    if (!journal_reset) {
        sqlite3_close_v2(destination_database);
        return remove_temporary(temporary_path, journal_reset.error());
    }
    sqlite3_close_v2(destination_database);

    const int synchronized_descriptor =
        ::open(temporary_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (synchronized_descriptor < 0) {
        return remove_temporary(temporary_path, common::Error{common::ErrorCode::database_error,
                                                              std::string{description} +
                                                                  " temporary file reopen failed"});
    }
    auto synchronized = fsync_file(synchronized_descriptor, description);
    static_cast<void>(::close(synchronized_descriptor));
    if (!synchronized) {
        return remove_temporary(temporary_path, std::move(synchronized).error());
    }

    if (::rename(temporary_path.c_str(), std::string{destination}.c_str()) != 0) {
        return remove_temporary(
            temporary_path,
            common::Error{common::ErrorCode::database_error,
                          std::string{description} + " destination installation failed"});
    }
    if (auto synced = fsync_directory(destination, description); !synced) {
        // The backup is already atomically installed.  Report the durability
        // warning so callers can retry a directory sync if required.
        return synced;
    }
    return common::Result<void>::success();
}

namespace {

[[nodiscard]] common::Result<sqlite3*> open_validated_restore_source(
    const std::string_view destination_path, const std::string_view source,
    const std::string_view description,
    const std::function<common::Result<void>(sqlite3*)>& source_validator) {
    if (source.empty() || source.size() > kMaximumDatabasePathBytes ||
        source.find('\0') != std::string_view::npos || destination_path.empty() ||
        destination_path.size() > kMaximumDatabasePathBytes ||
        destination_path.find('\0') != std::string_view::npos) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{description} + " source path is invalid"};
    }
    if (source == destination_path) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{description} + " source must differ from destination"};
    }
    if (auto parent = validate_private_parent(source, description); !parent) {
        return parent.error();
    }
    struct stat source_status{};
    if (::lstat(std::string{source}.c_str(), &source_status) != 0 ||
        !S_ISREG(source_status.st_mode)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{description} + " source must be a regular file"};
    }
    if (source_status.st_uid != ::geteuid() || (source_status.st_mode & 0077) != 0) {
        return common::Error{common::ErrorCode::permission_denied,
                             std::string{description} + " source file permissions are unsafe"};
    }

    sqlite3* source_database = nullptr;
    const int open_result = sqlite3_open_v2(std::string{source}.c_str(), &source_database,
                                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (open_result != SQLITE_OK) {
        common::Error error =
            sqlite_error(source_database, open_result, std::string{description} + " source open");
        if (source_database != nullptr) {
            sqlite3_close_v2(source_database);
        }
        return error;
    }
    sqlite3_extended_result_codes(source_database, 1);
    sqlite3_busy_timeout(source_database, 5'000);

    auto integrity = query_single_text(source_database, "PRAGMA integrity_check",
                                       std::string{description} + " source integrity");
    if (!integrity || *integrity != "ok") {
        common::Error error =
            integrity ? common::Error{common::ErrorCode::database_error,
                                      std::string{description} + " source integrity check failed"}
                      : integrity.error();
        sqlite3_close_v2(source_database);
        return error;
    }
    if (source_validator) {
        auto validated = source_validator(source_database);
        if (!validated) {
            sqlite3_close_v2(source_database);
            return validated.error();
        }
    }
    return source_database;
}

} // namespace

common::Result<void>
validate_restore_source(const std::string_view destination_path, const std::string_view source,
                        const std::string_view description,
                        const std::function<common::Result<void>(sqlite3*)>& source_validator) {
    auto source_database =
        open_validated_restore_source(destination_path, source, description, source_validator);
    if (!source_database) {
        return source_database.error();
    }
    sqlite3_close_v2(*source_database);
    return common::Result<void>::success();
}

common::Result<void>
restore_database(sqlite3* destination_database, const std::string_view destination_path,
                 const std::string_view source, const std::string_view description,
                 const std::function<common::Result<void>(sqlite3*)>& source_validator) {
    if (destination_database == nullptr) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{description} + " source path is invalid"};
    }
    auto opened_source =
        open_validated_restore_source(destination_path, source, description, source_validator);
    if (!opened_source) {
        return opened_source.error();
    }
    sqlite3* const source_database = *opened_source;

    sqlite3_backup* backup =
        sqlite3_backup_init(destination_database, "main", source_database, "main");
    if (backup == nullptr) {
        common::Error error =
            sqlite_error(destination_database, sqlite3_errcode(destination_database),
                         std::string{description} + " initialize restore");
        sqlite3_close_v2(source_database);
        return error;
    }
    int step_result = SQLITE_OK;
    int busy_steps = 0;
    constexpr int kPagesPerStep = 256;
    constexpr int kMaximumBusySteps = 200;
    while (true) {
        step_result = sqlite3_backup_step(backup, kPagesPerStep);
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result == SQLITE_OK) {
            busy_steps = 0;
            continue;
        }
        if (step_result == SQLITE_BUSY || step_result == SQLITE_LOCKED) {
            if (++busy_steps > kMaximumBusySteps) {
                break;
            }
            sqlite3_sleep(25);
            continue;
        }
        break;
    }
    const int finish_result = sqlite3_backup_finish(backup);
    sqlite3_close_v2(source_database);
    if (step_result != SQLITE_DONE || finish_result != SQLITE_OK) {
        const int error_code = finish_result != SQLITE_OK ? finish_result : step_result;
        return sqlite_error(destination_database, error_code,
                            std::string{description} + " copy database pages");
    }

    int log_frames = -1;
    int checkpointed_frames = -1;
    const int checkpoint_result =
        sqlite3_wal_checkpoint_v2(destination_database, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                  &log_frames, &checkpointed_frames);
    if (checkpoint_result != SQLITE_OK && checkpoint_result != SQLITE_ERROR) {
        return sqlite_error(destination_database, checkpoint_result,
                            std::string{description} + " checkpoint restored database");
    }
    struct stat destination_status{};
    if (::stat(std::string{destination_path}.c_str(), &destination_status) == 0) {
        const int descriptor = ::open(std::string{destination_path}.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor >= 0) {
            static_cast<void>(::fsync(descriptor));
            static_cast<void>(::close(descriptor));
        }
    }
    return common::Result<void>::success();
}

common::Result<std::int64_t> query_single_int64(sqlite3* database, const std::string_view sql,
                                                const std::string_view operation) {
    auto statement = Statement::prepare(database, sql, operation);
    if (!statement) {
        return std::move(statement).error();
    }
    auto step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != StepResult::row || sqlite3_column_type(statement->handle(), 0) != SQLITE_INTEGER) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{operation} + ": expected one integer row"};
    }
    const std::int64_t value = sqlite3_column_int64(statement->handle(), 0);
    step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != StepResult::done) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{operation} + ": expected exactly one row"};
    }
    return value;
}

common::Result<std::string> query_single_text(sqlite3* database, const std::string_view sql,
                                              const std::string_view operation) {
    auto statement = Statement::prepare(database, sql, operation);
    if (!statement) {
        return std::move(statement).error();
    }
    auto step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != StepResult::row) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{operation} + ": expected one text row"};
    }
    auto value = required_text(statement->handle(), 0, operation);
    if (!value) {
        return std::move(value).error();
    }
    step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != StepResult::done) {
        return common::Error{common::ErrorCode::database_error,
                             std::string{operation} + ": expected exactly one row"};
    }
    return value;
}

common::Result<std::string> required_text(sqlite3_stmt* statement, const int column,
                                          const std::string_view field) {
    if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
        return corrupt_field(field);
    }
    const auto* const value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (value == nullptr || bytes < 0) {
        return corrupt_field(field);
    }
    return std::string{reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes)};
}

common::Result<std::optional<std::string>> optional_text(sqlite3_stmt* statement, const int column,
                                                         const std::string_view field) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::optional<std::string>{};
    }
    auto value = required_text(statement, column, field);
    if (!value) {
        return std::move(value).error();
    }
    return std::optional<std::string>{std::move(*value)};
}

common::Result<std::int64_t> required_int64(sqlite3_stmt* statement, const int column,
                                            const std::string_view field) {
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
        return corrupt_field(field);
    }
    return sqlite3_column_int64(statement, column);
}

common::Result<std::optional<std::int64_t>>
optional_int64(sqlite3_stmt* statement, const int column, const std::string_view field) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::optional<std::int64_t>{};
    }
    auto value = required_int64(statement, column, field);
    if (!value) {
        return std::move(value).error();
    }
    return std::optional<std::int64_t>{*value};
}

common::Result<void> validate_text(const std::string_view value, const std::size_t minimum_bytes,
                                   const std::size_t maximum_bytes, const std::string_view field) {
    if (value.size() < minimum_bytes || value.size() > maximum_bytes) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{field} + " is outside its byte-length limit"};
    }
    if (value.find('\0') != std::string_view::npos) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{field} + " must not contain NUL bytes"};
    }
    if (!is_valid_utf8(value)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             std::string{field} + " must contain valid UTF-8"};
    }
    return common::Result<void>::success();
}

common::Result<void> validate_server_record(const ServerRecord& record) {
    if (record.id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "server record ID must use the server ID namespace"};
    }
    if (auto result = validate_optional_text(record.name, 1U, kMaxNameBytes, "server name");
        !result) {
        return result;
    }
    if (auto result = validate_optional_text(record.credential_ref, 1U,
                                             kMaxCredentialReferenceBytes, "credential reference");
        !result) {
        return result;
    }
    if (auto result = validate_optional_text(record.remote_server_id, 1U, kMaxRemoteServerIdBytes,
                                             "remote server ID");
        !result) {
        return result;
    }
    if (auto result = validate_optional_text(record.tls_server_name, 1U, kMaxTlsServerNameBytes,
                                             "TLS server name");
        !result) {
        return result;
    }
    constexpr std::string_view reference_fields[]{
        "CA credential reference",
        "client certificate reference",
        "client private-key reference",
    };
    const std::optional<std::string>* references[]{
        &record.ca_credential_ref,
        &record.client_certificate_ref,
        &record.client_private_key_ref,
    };
    for (std::size_t index = 0U; index < std::size(references); ++index) {
        if (auto result = validate_optional_text(
                *references[index], 1U, kMaxCredentialReferenceBytes, reference_fields[index]);
            !result) {
            return result;
        }
    }
    if (record.client_certificate_ref.has_value() != record.client_private_key_ref.has_value()) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "client certificate and private-key references must be paired"};
    }
    if (record.config_revision == 0U ||
        record.config_revision >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "server configuration revision is outside the storage limit"};
    }
    if (!is_known_enum(record.desired_state, server_desired_state_from_string) ||
        !is_known_enum(record.actual_state, server_actual_state_from_string)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "server record contains an unknown state"};
    }
    if (auto result = validate_error_fields(record.last_error_code, record.last_error_message);
        !result) {
        return result;
    }
    if (record.reconnect_attempt >
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "reconnect_attempt exceeds the storage limit"};
    }
    if (record.latency_ms.has_value() &&
        (*record.latency_ms < 0 || *record.latency_ms > std::numeric_limits<std::int32_t>::max())) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "latency_ms exceeds the storage limit"};
    }
    if (record.created_at_unix_ms < 0 || record.updated_at_unix_ms < record.created_at_unix_ms) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "server timestamps must be non-negative and monotonic"};
    }
    return common::Result<void>::success();
}

common::Result<void> validate_tunnel_record(const TunnelRecord& record) {
    if (record.id.kind() != common::IdKind::tunnel ||
        record.server_id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel record contains an ID from the wrong namespace"};
    }
    if (auto result = validate_optional_text(record.name, 1U, kMaxNameBytes, "tunnel name");
        !result) {
        return result;
    }
    if (!is_known_enum(record.protocol, tunnel_protocol_from_string) ||
        !is_known_enum(record.desired_state, tunnel_desired_state_from_string) ||
        !is_known_enum(record.actual_state, tunnel_actual_state_from_string)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel record contains an unknown protocol or state"};
    }
    if (record.protocol != TunnelProtocol::tcp) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "stage 2 persistence only supports TCP tunnels"};
    }
    if (auto result = validate_error_fields(record.last_error_code, record.last_error_message);
        !result) {
        return result;
    }
    if (record.config_revision == 0U ||
        record.config_revision >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel configuration revision is outside the storage limit"};
    }
    if (record.created_at_unix_ms < 0 || record.updated_at_unix_ms < record.created_at_unix_ms ||
        (record.last_synced_at_unix_ms.has_value() &&
         (*record.last_synced_at_unix_ms < record.created_at_unix_ms ||
          *record.last_synced_at_unix_ms > record.updated_at_unix_ms))) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel timestamps must be non-negative and monotonic"};
    }
    return common::Result<void>::success();
}

common::Result<ServerRecord> read_server(sqlite3_stmt* statement) {
    auto id_text = required_text(statement, 0, "servers.id");
    auto name = optional_text(statement, 1, "servers.name");
    auto endpoint_value = required_text(statement, 2, "servers.endpoint");
    auto credential_ref = optional_text(statement, 3, "servers.credential_ref");
    auto remote_server_id = optional_text(statement, 4, "servers.remote_server_id");
    auto desired_text = required_text(statement, 5, "servers.desired_state");
    auto actual_text = required_text(statement, 6, "servers.actual_state");
    auto error_text = optional_text(statement, 7, "servers.last_error_code");
    auto error_message = optional_text(statement, 8, "servers.last_error_message");
    auto reconnect_attempt = required_int64(statement, 9, "servers.reconnect_attempt");
    auto latency = optional_int64(statement, 10, "servers.latency_ms");
    auto created_at = required_int64(statement, 11, "servers.created_at");
    auto updated_at = required_int64(statement, 12, "servers.updated_at");
    auto tls_server_name = optional_text(statement, 13, "servers.tls_server_name");
    auto ca_credential_ref = optional_text(statement, 14, "servers.ca_credential_ref");
    auto client_certificate_ref = optional_text(statement, 15, "servers.client_certificate_ref");
    auto client_private_key_ref = optional_text(statement, 16, "servers.client_private_key_ref");
    auto config_revision = required_int64(statement, 17, "servers.config_revision");
    auto managed_by_config = required_int64(statement, 18, "servers.managed_by_config");

    if (!id_text || !name || !endpoint_value || !credential_ref || !remote_server_id ||
        !desired_text || !actual_text || !error_text || !error_message || !reconnect_attempt ||
        !latency || !created_at || !updated_at || !tls_server_name || !ca_credential_ref ||
        !client_certificate_ref || !client_private_key_ref || !config_revision ||
        !managed_by_config) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a malformed server row"};
    }

    auto id = common::Id::parse(*id_text, common::IdKind::server);
    auto endpoint = read_endpoint(*endpoint_value, "servers.endpoint");
    const auto desired = server_desired_state_from_string(*desired_text);
    const auto actual = server_actual_state_from_string(*actual_text);
    if (!id || !endpoint || !desired.has_value() || !actual.has_value() || *reconnect_attempt < 0 ||
        *reconnect_attempt > std::numeric_limits<std::int32_t>::max() ||
        (latency->has_value() &&
         (**latency < 0 || **latency > std::numeric_limits<std::int32_t>::max())) ||
        *config_revision <= 0 || (*managed_by_config != 0 && *managed_by_config != 1)) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains an invalid server row"};
    }

    std::optional<common::ErrorCode> error_code;
    if (error_text->has_value()) {
        error_code = common::error_code_from_string(**error_text);
        if (!error_code.has_value() || !is_known_error_code(*error_code)) {
            return corrupt_field("servers.last_error_code");
        }
    }

    ServerRecord record{
        .id = std::move(*id),
        .name = std::move(*name),
        .endpoint = std::move(*endpoint),
        .credential_ref = std::move(*credential_ref),
        .remote_server_id = std::move(*remote_server_id),
        .desired_state = *desired,
        .actual_state = *actual,
        .last_error_code = error_code,
        .last_error_message = std::move(*error_message),
        .reconnect_attempt = static_cast<std::uint32_t>(*reconnect_attempt),
        .latency_ms = *latency,
        .created_at_unix_ms = *created_at,
        .updated_at_unix_ms = *updated_at,
        .tls_server_name = std::move(*tls_server_name),
        .ca_credential_ref = std::move(*ca_credential_ref),
        .client_certificate_ref = std::move(*client_certificate_ref),
        .client_private_key_ref = std::move(*client_private_key_ref),
        .config_revision = static_cast<std::uint64_t>(*config_revision),
        .managed_by_config = *managed_by_config != 0,
    };
    auto validated = validate_server_record(record);
    if (!validated) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a server row outside storage limits"};
    }
    return record;
}

common::Result<TunnelRecord> read_tunnel(sqlite3_stmt* statement) {
    auto id_text = required_text(statement, 0, "tunnels.id");
    auto name = optional_text(statement, 1, "tunnels.name");
    auto server_id_text = required_text(statement, 2, "tunnels.server_id");
    auto protocol_text = required_text(statement, 3, "tunnels.protocol");
    auto local_host = required_text(statement, 4, "tunnels.local_host");
    auto local_port = required_int64(statement, 5, "tunnels.local_port");
    auto remote_host = required_text(statement, 6, "tunnels.remote_host");
    auto remote_port = required_int64(statement, 7, "tunnels.remote_port");
    auto desired_text = required_text(statement, 8, "tunnels.desired_state");
    auto actual_text = required_text(statement, 9, "tunnels.actual_state");
    auto error_text = optional_text(statement, 10, "tunnels.last_error_code");
    auto error_message = optional_text(statement, 11, "tunnels.last_error_message");
    auto created_at = required_int64(statement, 12, "tunnels.created_at");
    auto updated_at = required_int64(statement, 13, "tunnels.updated_at");
    auto last_synced_at = optional_int64(statement, 14, "tunnels.last_synced_at");
    auto config_revision = required_int64(statement, 15, "tunnels.config_revision");
    auto managed_by_config = required_int64(statement, 16, "tunnels.managed_by_config");

    if (!id_text || !name || !server_id_text || !protocol_text || !local_host || !local_port ||
        !remote_host || !remote_port || !desired_text || !actual_text || !error_text ||
        !error_message || !created_at || !updated_at || !last_synced_at || !config_revision ||
        !managed_by_config) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a malformed tunnel row"};
    }

    auto id = common::Id::parse(*id_text, common::IdKind::tunnel);
    auto server_id = common::Id::parse(*server_id_text, common::IdKind::server);
    auto local_endpoint = read_split_endpoint(*local_host, *local_port, "tunnels.local_endpoint");
    auto remote_endpoint =
        read_split_endpoint(*remote_host, *remote_port, "tunnels.remote_endpoint");
    const auto protocol = tunnel_protocol_from_string(*protocol_text);
    const auto desired = tunnel_desired_state_from_string(*desired_text);
    const auto actual = tunnel_actual_state_from_string(*actual_text);
    if (!id || !server_id || !local_endpoint || !remote_endpoint || !protocol.has_value() ||
        !desired.has_value() || !actual.has_value() || *config_revision <= 0 ||
        (*managed_by_config != 0 && *managed_by_config != 1)) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains an invalid tunnel row"};
    }

    std::optional<common::ErrorCode> error_code;
    if (error_text->has_value()) {
        error_code = common::error_code_from_string(**error_text);
        if (!error_code.has_value() || !is_known_error_code(*error_code)) {
            return corrupt_field("tunnels.last_error_code");
        }
    }

    TunnelRecord record{
        .id = std::move(*id),
        .name = std::move(*name),
        .server_id = std::move(*server_id),
        .protocol = *protocol,
        .local_endpoint = std::move(*local_endpoint),
        .remote_endpoint = std::move(*remote_endpoint),
        .desired_state = *desired,
        .actual_state = *actual,
        .last_error_code = error_code,
        .last_error_message = std::move(*error_message),
        .created_at_unix_ms = *created_at,
        .updated_at_unix_ms = *updated_at,
        .last_synced_at_unix_ms = *last_synced_at,
        .config_revision = static_cast<std::uint64_t>(*config_revision),
        .managed_by_config = *managed_by_config != 0,
    };
    auto validated = validate_tunnel_record(record);
    if (!validated) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a tunnel row outside storage limits"};
    }
    return record;
}

std::string endpoint_text(const std::string_view host, const std::uint16_t port) {
    if (host.find(':') != std::string_view::npos) {
        return '[' + std::string{host} + "]:" + std::to_string(port);
    }
    return std::string{host} + ':' + std::to_string(port);
}

} // namespace minitun::storage::internal
