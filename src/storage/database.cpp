#include <minitun/storage/database.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <sqlite3.h>

#include <minitun/common/error.hpp>
#include <minitun/common/time.hpp>

#include "file_security.hpp"
#include "sqlite_internal.hpp"

namespace minitun::storage {
namespace {

constexpr std::string_view kCreateSchemaVersion = R"sql(
CREATE TABLE schema_version (
    version INTEGER PRIMARY KEY
        CHECK(typeof(version) = 'integer' AND version >= 1),
    applied_at INTEGER NOT NULL
        CHECK(typeof(applied_at) = 'integer' AND applied_at >= 0)
)
)sql";

constexpr std::string_view kCreateServers = R"sql(
CREATE TABLE servers (
    id TEXT NOT NULL PRIMARY KEY
        CHECK(
            typeof(id) = 'text'
            AND length(CAST(id AS BLOB)) = 36
            AND substr(id, 1, 4) = 'srv_'
            AND substr(id, 5) NOT GLOB '*[^0-9a-f]*'
        ),
    name TEXT UNIQUE
        CHECK(
            name IS NULL OR (
                typeof(name) = 'text'
                AND length(CAST(name AS BLOB)) BETWEEN 1 AND 64
            )
        ),
    endpoint TEXT NOT NULL
        CHECK(
            typeof(endpoint) = 'text'
            AND length(CAST(endpoint AS BLOB)) BETWEEN 1 AND 260
        ),
    credential_ref TEXT
        CHECK(
            credential_ref IS NULL OR (
                typeof(credential_ref) = 'text'
                AND length(CAST(credential_ref AS BLOB)) BETWEEN 1 AND 256
            )
        ),
    remote_server_id TEXT
        CHECK(
            remote_server_id IS NULL OR (
                typeof(remote_server_id) = 'text'
                AND length(CAST(remote_server_id AS BLOB)) BETWEEN 1 AND 256
            )
        ),

    desired_state TEXT NOT NULL
        CHECK(desired_state IN ('enabled', 'disabled', 'removed')),
    actual_state TEXT NOT NULL
        CHECK(actual_state IN (
            'not_authenticated', 'disconnected', 'connecting',
            'tls_handshake', 'authenticating', 'online',
            'backoff', 'disabled', 'error'
        )),

    last_error_code TEXT
        CHECK(last_error_code IS NULL OR last_error_code IN (
            'invalid_argument', 'not_found', 'already_exists',
            'permission_denied', 'not_authenticated',
            'authentication_failed', 'connection_failed',
            'connection_timeout', 'remote_port_in_use',
            'local_connect_failed', 'protocol_error', 'frame_too_large',
            'unsupported_version', 'resource_exhausted', 'database_error',
            'tls_error', 'ipc_error', 'internal_error'
        )),
    last_error_message TEXT
        CHECK(
            last_error_message IS NULL OR (
                typeof(last_error_message) = 'text'
                AND length(CAST(last_error_message AS BLOB)) <= 4096
            )
        ),

    reconnect_attempt INTEGER NOT NULL DEFAULT 0
        CHECK(
            typeof(reconnect_attempt) = 'integer'
            AND reconnect_attempt BETWEEN 0 AND 2147483647
        ),
    latency_ms INTEGER
        CHECK(
            latency_ms IS NULL OR (
                typeof(latency_ms) = 'integer'
                AND latency_ms BETWEEN 0 AND 2147483647
            )
        ),

    created_at INTEGER NOT NULL
        CHECK(typeof(created_at) = 'integer' AND created_at >= 0),
    updated_at INTEGER NOT NULL
        CHECK(
            typeof(updated_at) = 'integer'
            AND updated_at >= created_at
        )
)
)sql";

constexpr std::string_view kCreateTunnels = R"sql(
CREATE TABLE tunnels (
    id TEXT NOT NULL PRIMARY KEY
        CHECK(
            typeof(id) = 'text'
            AND length(CAST(id AS BLOB)) = 36
            AND substr(id, 1, 4) = 'tun_'
            AND substr(id, 5) NOT GLOB '*[^0-9a-f]*'
        ),
    name TEXT
        CHECK(
            name IS NULL OR (
                typeof(name) = 'text'
                AND length(CAST(name AS BLOB)) BETWEEN 1 AND 64
            )
        ),
    server_id TEXT NOT NULL
        CHECK(
            typeof(server_id) = 'text'
            AND length(CAST(server_id AS BLOB)) = 36
            AND substr(server_id, 1, 4) = 'srv_'
            AND substr(server_id, 5) NOT GLOB '*[^0-9a-f]*'
        ),

    protocol TEXT NOT NULL CHECK(protocol = 'tcp'),
    local_host TEXT NOT NULL
        CHECK(
            typeof(local_host) = 'text'
            AND length(CAST(local_host AS BLOB)) BETWEEN 1 AND 254
        ),
    local_port INTEGER NOT NULL
        CHECK(
            typeof(local_port) = 'integer'
            AND local_port BETWEEN 1 AND 65535
        ),

    remote_host TEXT NOT NULL
        CHECK(
            typeof(remote_host) = 'text'
            AND length(CAST(remote_host AS BLOB)) BETWEEN 1 AND 254
        ),
    remote_port INTEGER NOT NULL
        CHECK(
            typeof(remote_port) = 'integer'
            AND remote_port BETWEEN 1 AND 65535
        ),

    desired_state TEXT NOT NULL
        CHECK(desired_state IN ('active', 'disabled', 'removed')),
    actual_state TEXT NOT NULL
        CHECK(actual_state IN (
            'pending', 'registering', 'active',
            'failed', 'removing', 'disabled'
        )),

    last_error_code TEXT
        CHECK(last_error_code IS NULL OR last_error_code IN (
            'invalid_argument', 'not_found', 'already_exists',
            'permission_denied', 'not_authenticated',
            'authentication_failed', 'connection_failed',
            'connection_timeout', 'remote_port_in_use',
            'local_connect_failed', 'protocol_error', 'frame_too_large',
            'unsupported_version', 'resource_exhausted', 'database_error',
            'tls_error', 'ipc_error', 'internal_error'
        )),
    last_error_message TEXT
        CHECK(
            last_error_message IS NULL OR (
                typeof(last_error_message) = 'text'
                AND length(CAST(last_error_message AS BLOB)) <= 4096
            )
        ),

    created_at INTEGER NOT NULL
        CHECK(typeof(created_at) = 'integer' AND created_at >= 0),
    updated_at INTEGER NOT NULL
        CHECK(
            typeof(updated_at) = 'integer'
            AND updated_at >= created_at
        ),

    FOREIGN KEY(server_id)
        REFERENCES servers(id)
        ON DELETE CASCADE,

    UNIQUE(server_id, protocol, remote_host, remote_port)
)
)sql";

constexpr std::string_view kCreateServerReconcileIndex =
    "CREATE INDEX idx_servers_reconcile ON servers(desired_state, id)";
constexpr std::string_view kCreateTunnelReconcileIndex =
    "CREATE INDEX idx_tunnels_reconcile ON tunnels(server_id, desired_state, id)";
constexpr std::string_view kCreateTunnelNameIndex =
    "CREATE INDEX idx_tunnels_name ON tunnels(name) WHERE name IS NOT NULL";

constexpr std::string_view kCreateDaemonIdentity = R"sql(
CREATE TABLE daemon_identity (
    singleton INTEGER NOT NULL PRIMARY KEY
        CHECK(typeof(singleton) = 'integer' AND singleton = 1),
    client_id TEXT NOT NULL UNIQUE
        CHECK(
            typeof(client_id) = 'text'
            AND length(CAST(client_id AS BLOB)) = 39
            AND substr(client_id, 1, 7) = 'client_'
            AND substr(client_id, 8) NOT GLOB '*[^0-9a-f]*'
        )
)
)sql";

[[nodiscard]] common::Error migration_error(const common::Error& error) {
    return common::Error{common::ErrorCode::database_error, error.message()};
}

[[nodiscard]] bool is_sql_whitespace(const char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f' || character == '\v';
}

[[nodiscard]] std::string normalize_schema_sql(const std::string_view sql) {
    std::string normalized;
    normalized.reserve(sql.size());
    bool pending_space = false;
    for (const char character : sql) {
        if (is_sql_whitespace(character)) {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(character);
    }
    return normalized;
}

[[nodiscard]] common::Result<void> validate_schema_object(sqlite3* database,
                                                          const std::string_view type,
                                                          const std::string_view name,
                                                          const std::string_view expected_sql) {
    auto statement = internal::Statement::prepare(
        database, "SELECT sql FROM sqlite_master WHERE type = ?1 AND name = ?2",
        "validate SQLite schema definition");
    if (!statement) {
        return migration_error(statement.error());
    }
    if (auto bound = statement->bind_text(1, type); !bound) {
        return migration_error(bound.error());
    }
    if (auto bound = statement->bind_text(2, name); !bound) {
        return migration_error(bound.error());
    }

    auto step = statement->step();
    if (!step) {
        return migration_error(step.error());
    }
    if (*step != internal::StepResult::row) {
        return common::Error{common::ErrorCode::database_error,
                             "database is missing a required schema object"};
    }
    auto actual_sql = internal::required_text(statement->handle(), 0, "schema object SQL");
    if (!actual_sql) {
        return migration_error(actual_sql.error());
    }
    if (normalize_schema_sql(*actual_sql) != normalize_schema_sql(expected_sql)) {
        return common::Error{common::ErrorCode::database_error,
                             "database schema definition does not match its version"};
    }

    step = statement->step();
    if (!step || *step != internal::StepResult::done) {
        return !step ? migration_error(step.error())
                     : common::Error{common::ErrorCode::database_error,
                                     "database contains duplicate schema objects"};
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<bool> table_exists(sqlite3* database,
                                                const std::string_view table_name) {
    auto statement = internal::Statement::prepare(
        database, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?1",
        "inspect SQLite schema");
    if (!statement) {
        return std::move(statement).error();
    }
    if (auto result = statement->bind_text(1, table_name); !result) {
        return result.error();
    }
    auto step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != internal::StepResult::row ||
        sqlite3_column_type(statement->handle(), 0) != SQLITE_INTEGER) {
        return common::Error{common::ErrorCode::database_error,
                             "SQLite schema inspection returned an invalid row"};
    }
    const bool exists = sqlite3_column_int64(statement->handle(), 0) == 1;
    step = statement->step();
    if (!step || *step != internal::StepResult::done) {
        return common::Error{common::ErrorCode::database_error,
                             "SQLite schema inspection returned extra rows"};
    }
    return exists;
}

[[nodiscard]] common::Result<std::int64_t> user_schema_object_count(sqlite3* database) {
    return internal::query_single_int64(database,
                                        "SELECT COUNT(*) FROM sqlite_master "
                                        "WHERE name NOT GLOB 'sqlite_*'",
                                        "count SQLite user schema objects");
}

[[nodiscard]] common::Result<int> read_schema_version(sqlite3* database,
                                                      const bool require_non_empty) {
    auto statement = internal::Statement::prepare(
        database, "SELECT version FROM schema_version ORDER BY version",
        "read SQLite schema history");
    if (!statement) {
        return std::move(statement).error();
    }

    int expected = 1;
    int current = 0;
    while (true) {
        auto step = statement->step();
        if (!step) {
            return std::move(step).error();
        }
        if (*step == internal::StepResult::done) {
            break;
        }
        if (sqlite3_column_type(statement->handle(), 0) != SQLITE_INTEGER) {
            return common::Error{common::ErrorCode::database_error,
                                 "schema_version contains a non-integer version"};
        }
        const std::int64_t version = sqlite3_column_int64(statement->handle(), 0);
        if (version > kCurrentSchemaVersion) {
            return common::Error{common::ErrorCode::unsupported_version,
                                 "database schema is newer than this MiniTun build"};
        }
        if (version != expected) {
            return common::Error{common::ErrorCode::database_error,
                                 "database schema history is not contiguous"};
        }
        current = expected;
        ++expected;
    }

    if (require_non_empty && current == 0) {
        return common::Error{common::ErrorCode::database_error,
                             "schema_version exists but contains no migration history"};
    }
    return current;
}

[[nodiscard]] common::Result<void> validate_current_schema(sqlite3* database) {
    constexpr std::tuple<std::string_view, std::string_view, std::string_view> schema_objects[]{
        {"table", "schema_version", kCreateSchemaVersion},
        {"table", "servers", kCreateServers},
        {"table", "tunnels", kCreateTunnels},
        {"table", "daemon_identity", kCreateDaemonIdentity},
        {"index", "idx_servers_reconcile", kCreateServerReconcileIndex},
        {"index", "idx_tunnels_reconcile", kCreateTunnelReconcileIndex},
        {"index", "idx_tunnels_name", kCreateTunnelNameIndex},
    };
    for (const auto& [type, name, expected_sql] : schema_objects) {
        if (auto validated = validate_schema_object(database, type, name, expected_sql);
            !validated) {
            return validated;
        }
    }

    auto unexpected_objects = internal::query_single_int64(
        database,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE name NOT GLOB 'sqlite_*' AND ("
        "  type IN ('view', 'trigger') OR "
        "  (type = 'table' AND name NOT IN ("
        "    'schema_version', 'servers', 'tunnels', 'daemon_identity'"
        "  )) OR "
        "  (type = 'index' AND sql IS NOT NULL AND name NOT IN ("
        "    'idx_servers_reconcile', 'idx_tunnels_reconcile', 'idx_tunnels_name'"
        "  ))"
        ")",
        "detect unexpected SQLite schema objects");
    if (!unexpected_objects) {
        return migration_error(unexpected_objects.error());
    }
    if (*unexpected_objects != 0) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains unexpected schema objects"};
    }

    auto servers = internal::Statement::prepare(
        database,
        "SELECT id, name, endpoint, credential_ref, remote_server_id, "
        "desired_state, actual_state, last_error_code, last_error_message, "
        "reconnect_attempt, latency_ms, created_at, updated_at "
        "FROM servers LIMIT 0",
        "validate servers schema");
    if (!servers) {
        return migration_error(servers.error());
    }

    auto tunnels = internal::Statement::prepare(
        database,
        "SELECT id, name, server_id, protocol, local_host, local_port, "
        "remote_host, remote_port, desired_state, actual_state, "
        "last_error_code, last_error_message, created_at, updated_at "
        "FROM tunnels LIMIT 0",
        "validate tunnels schema");
    if (!tunnels) {
        return migration_error(tunnels.error());
    }

    auto identity = internal::Statement::prepare(
        database, "SELECT singleton, client_id FROM daemon_identity LIMIT 0",
        "validate daemon identity schema");
    if (!identity) {
        return migration_error(identity.error());
    }

    auto foreign_keys = internal::Statement::prepare(database, "PRAGMA foreign_key_list(tunnels)",
                                                     "validate tunnels foreign key");
    if (!foreign_keys) {
        return migration_error(foreign_keys.error());
    }

    bool found_cascade = false;
    while (true) {
        auto step = foreign_keys->step();
        if (!step) {
            return migration_error(step.error());
        }
        if (*step == internal::StepResult::done) {
            break;
        }
        auto referenced_table =
            internal::required_text(foreign_keys->handle(), 2, "foreign key table");
        auto source_column =
            internal::required_text(foreign_keys->handle(), 3, "foreign key source column");
        auto target_column =
            internal::required_text(foreign_keys->handle(), 4, "foreign key target column");
        auto delete_action =
            internal::required_text(foreign_keys->handle(), 6, "foreign key delete action");
        if (!referenced_table || !source_column || !target_column || !delete_action) {
            return common::Error{common::ErrorCode::database_error,
                                 "tunnels foreign-key metadata is malformed"};
        }
        if (*referenced_table == "servers" && *source_column == "server_id" &&
            *target_column == "id" && *delete_action == "CASCADE") {
            found_cascade = true;
        }
    }
    if (!found_cascade) {
        return common::Error{common::ErrorCode::database_error,
                             "tunnels schema is missing its cascading server foreign key"};
    }

    auto foreign_key_check = internal::Statement::prepare(database, "PRAGMA foreign_key_check",
                                                          "check persisted foreign keys");
    if (!foreign_key_check) {
        return migration_error(foreign_key_check.error());
    }
    auto step = foreign_key_check->step();
    if (!step) {
        return migration_error(step.error());
    }
    if (*step != internal::StepResult::done) {
        return common::Error{common::ErrorCode::database_error,
                             "database contains a foreign-key violation"};
    }

    const auto index_count = internal::query_single_int64(
        database,
        "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name IN ("
        "'idx_servers_reconcile', 'idx_tunnels_reconcile', 'idx_tunnels_name')",
        "validate storage indexes");
    if (!index_count) {
        return migration_error(index_count.error());
    }
    if (*index_count != 3) {
        return common::Error{common::ErrorCode::database_error,
                             "database is missing required storage indexes"};
    }

    auto integrity = internal::query_single_text(database, "PRAGMA integrity_check(1)",
                                                 "validate SQLite database integrity");
    if (!integrity) {
        return migration_error(integrity.error());
    }
    if (*integrity != "ok") {
        return common::Error{common::ErrorCode::database_error,
                             "database failed its SQLite integrity check"};
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> apply_version_one(sqlite3* database) {
    constexpr std::pair<std::string_view, std::string_view> statements[]{
        {kCreateServers, "create servers table"},
        {kCreateTunnels, "create tunnels table"},
        {kCreateServerReconcileIndex, "create server reconciliation index"},
        {kCreateTunnelReconcileIndex, "create tunnel reconciliation index"},
        {kCreateTunnelNameIndex, "create tunnel name index"},
    };

    for (const auto& [sql, operation] : statements) {
        auto result = internal::execute(database, sql, operation);
        if (!result) {
            return migration_error(result.error());
        }
    }

    auto insert = internal::Statement::prepare(
        database, "INSERT INTO schema_version(version, applied_at) VALUES(1, ?1)",
        "record schema migration");
    if (!insert) {
        return migration_error(insert.error());
    }
    if (auto result = insert->bind_int64(1, common::unix_milliseconds_now()); !result) {
        return migration_error(result.error());
    }
    auto step = insert->step();
    if (!step || *step != internal::StepResult::done) {
        return !step ? migration_error(step.error())
                     : common::Error{common::ErrorCode::database_error,
                                     "schema migration did not complete"};
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> apply_version_two(sqlite3* database) {
    auto created =
        internal::execute(database, kCreateDaemonIdentity, "create daemon identity table");
    if (!created) {
        return migration_error(created.error());
    }

    auto insert = internal::Statement::prepare(
        database, "INSERT INTO schema_version(version, applied_at) VALUES(2, ?1)",
        "record schema migration");
    if (!insert) {
        return migration_error(insert.error());
    }
    if (auto result = insert->bind_int64(1, common::unix_milliseconds_now()); !result) {
        return migration_error(result.error());
    }
    auto step = insert->step();
    if (!step || *step != internal::StepResult::done) {
        return !step ? migration_error(step.error())
                     : common::Error{common::ErrorCode::database_error,
                                     "schema migration did not complete"};
    }
    return common::Result<void>::success();
}

} // namespace

Transaction::Transaction(Database& database, std::unique_lock<std::recursive_mutex> lock) noexcept
    : database_(&database), lock_(std::move(lock)), active_(true) {}

Transaction::~Transaction() noexcept { rollback_noexcept(); }

Transaction::Transaction(Transaction&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)), lock_(std::move(other.lock_)),
      failure_(std::move(other.failure_)), active_(std::exchange(other.active_, false)) {}

common::Result<void> Transaction::commit() {
    if (!active_ || database_ == nullptr) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "transaction is no longer active"};
    }

    if (failure_.has_value()) {
        const common::Error original = *failure_;
        auto rolled_back = rollback();
        if (!rolled_back) {
            return common::Error{
                original.code(),
                original.message() + "; the transaction rollback also failed",
            };
        }
        return original;
    }

    auto committed = internal::execute(database_->handle_, "COMMIT", "commit SQLite transaction");
    if (!committed) {
        const common::Error original = committed.error();
        failure_ = original;
        auto rolled_back = rollback();
        if (!rolled_back) {
            return common::Error{
                original.code(),
                original.message() + "; the failed commit could not be rolled back",
            };
        }
        return original;
    }

    database_->transaction_active_ = false;
    active_ = false;
    lock_.unlock();
    return common::Result<void>::success();
}

common::Result<void> Transaction::rollback() {
    if (!active_ || database_ == nullptr) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "transaction is no longer active"};
    }

    auto rolled_back =
        internal::execute(database_->handle_, "ROLLBACK", "roll back SQLite transaction");
    if (rolled_back || sqlite3_get_autocommit(database_->handle_) != 0) {
        database_->transaction_active_ = false;
        active_ = false;
        lock_.unlock();
    }
    return rolled_back;
}

bool Transaction::active() const noexcept { return active_; }

bool Transaction::failed() const noexcept { return failure_.has_value(); }

bool Transaction::belongs_to(const Database& database) const noexcept {
    return active_ && database_ == &database;
}

void Transaction::mark_failed(common::Error error) {
    if (!failure_.has_value()) {
        failure_ = std::move(error);
    }
}

void Transaction::rollback_noexcept() noexcept {
    if (!active_ || database_ == nullptr) {
        return;
    }
    const int result = sqlite3_exec(database_->handle_, "ROLLBACK", nullptr, nullptr, nullptr);
    if (result != SQLITE_OK && sqlite3_get_autocommit(database_->handle_) == 0) {
        database_->poisoned_ = true;
    }
    database_->transaction_active_ = false;
    active_ = false;
}

Database::Database(sqlite3* handle, std::string path) noexcept
    : handle_(handle), path_(std::move(path)) {}

Database::~Database() noexcept {
    std::scoped_lock lock{mutex_};
    if (handle_ != nullptr) {
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

common::Result<std::unique_ptr<Database>> Database::open(const std::string_view path) {
    if (path.empty() || path.size() > kMaxDatabasePathBytes ||
        path.find('\0') != std::string_view::npos) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "database path is empty, oversized, or contains a NUL byte"};
    }

    auto prepared = internal::prepare_private_database_file(path, "SQLite state database");
    if (!prepared) {
        return prepared.error();
    }

    sqlite3* handle = nullptr;
    std::string owned_path{path};
    const int result = sqlite3_open_v2(
        owned_path.c_str(), &handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK) {
        const common::Error error =
            internal::sqlite_error(handle, result, "open SQLite state database");
        if (handle != nullptr) {
            sqlite3_close_v2(handle);
        }
        return error;
    }
    if (auto verified = prepared->verify_path_identity(); !verified) {
        sqlite3_close_v2(handle);
        return verified.error();
    }

    sqlite3_extended_result_codes(handle, 1);
    auto database = std::unique_ptr<Database>{new Database{handle, std::move(owned_path)}};
    if (auto configured = database->configure(); !configured) {
        return configured.error();
    }
    if (auto migrated = database->migrate(); !migrated) {
        return migrated.error();
    }
    if (auto wal = database->enable_wal(); !wal) {
        return wal.error();
    }
    return database;
}

common::Result<void> Database::configure() {
    std::scoped_lock lock{mutex_};

    if (sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") == 0) {
        int extension_loading_enabled = 0;
        const int extension_result = sqlite3_db_config(
            handle_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, &extension_loading_enabled);
        if (extension_result != SQLITE_OK || extension_loading_enabled != 0) {
            return common::Error{common::ErrorCode::database_error,
                                 "failed to disable SQLite extension loading"};
        }
    }
    if (sqlite3_busy_timeout(handle_, kDatabaseBusyTimeoutMilliseconds) != SQLITE_OK) {
        return common::Error{common::ErrorCode::database_error,
                             "failed to configure SQLite busy timeout"};
    }

    constexpr std::pair<std::string_view, std::string_view> pragmas[]{
        {"PRAGMA foreign_keys = ON", "enable SQLite foreign keys"},
        {"PRAGMA synchronous = NORMAL", "configure SQLite synchronous mode"},
    };
    for (const auto& [sql, operation] : pragmas) {
        if (auto configured = internal::execute(handle_, sql, operation); !configured) {
            return configured;
        }
    }

    constexpr std::pair<std::string_view, std::int64_t> checks[]{
        {"PRAGMA foreign_keys", 1},
        {"PRAGMA synchronous", 1},
        {"PRAGMA busy_timeout", kDatabaseBusyTimeoutMilliseconds},
    };
    for (const auto& [sql, expected] : checks) {
        auto actual = internal::query_single_int64(handle_, sql, "verify SQLite configuration");
        if (!actual) {
            return std::move(actual).error();
        }
        if (*actual != expected) {
            return common::Error{common::ErrorCode::database_error,
                                 "SQLite connection rejected a required configuration"};
        }
    }
    return common::Result<void>::success();
}

common::Result<void> Database::enable_wal() {
    std::scoped_lock lock{mutex_};

    auto journal_mode =
        internal::query_single_text(handle_, "PRAGMA journal_mode = WAL", "enable SQLite WAL mode");
    if (!journal_mode) {
        return std::move(journal_mode).error();
    }
    if (*journal_mode != "wal") {
        return common::Error{common::ErrorCode::database_error,
                             "SQLite database does not support WAL mode"};
    }

    constexpr std::pair<std::string_view, std::string_view> pragmas[]{
        {"PRAGMA wal_autocheckpoint = 1000", "configure SQLite WAL checkpointing"},
        {"PRAGMA journal_size_limit = 16777216", "bound SQLite WAL journal size"},
    };
    for (const auto& [sql, operation] : pragmas) {
        if (auto configured = internal::execute(handle_, sql, operation); !configured) {
            return configured;
        }
    }

    constexpr std::pair<std::string_view, std::int64_t> checks[]{
        {"PRAGMA wal_autocheckpoint", kWalAutoCheckpointPages},
        {"PRAGMA journal_size_limit", kWalJournalSizeLimitBytes},
    };
    for (const auto& [sql, expected] : checks) {
        auto actual = internal::query_single_int64(handle_, sql, "verify SQLite WAL configuration");
        if (!actual) {
            return std::move(actual).error();
        }
        if (*actual != expected) {
            return common::Error{common::ErrorCode::database_error,
                                 "SQLite connection rejected a required WAL configuration"};
        }
    }
    return common::Result<void>::success();
}

common::Result<void> Database::migrate() {
    auto transaction = begin_transaction();
    if (!transaction) {
        return std::move(transaction).error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction->mark_failed(std::move(error));
        return transaction->commit();
    };

    auto has_schema_version = table_exists(handle_, "schema_version");
    if (!has_schema_version) {
        return fail(migration_error(has_schema_version.error()));
    }

    int version = 0;
    if (!*has_schema_version) {
        auto object_count = user_schema_object_count(handle_);
        if (!object_count) {
            return fail(migration_error(object_count.error()));
        }
        if (*object_count != 0) {
            return fail(common::Error{
                common::ErrorCode::database_error,
                "refusing to migrate an unversioned non-empty database",
            });
        }
        auto created =
            internal::execute(handle_, kCreateSchemaVersion, "create schema_version table");
        if (!created) {
            return fail(migration_error(created.error()));
        }
    } else {
        auto current = read_schema_version(handle_, true);
        if (!current) {
            return fail(current.error());
        }
        version = *current;
    }

    if (version == 0) {
        if (auto applied = apply_version_one(handle_); !applied) {
            return fail(applied.error());
        }
        version = 1;
    }
    if (version == 1) {
        if (auto applied = apply_version_two(handle_); !applied) {
            return fail(applied.error());
        }
        version = 2;
    }
    if (version != kCurrentSchemaVersion) {
        return fail(common::Error{
            common::ErrorCode::unsupported_version,
            "database schema version is unsupported",
        });
    }

    if (auto validated = validate_current_schema(handle_); !validated) {
        return fail(validated.error());
    }
    return transaction->commit();
}

common::Result<int> Database::schema_version() const {
    std::scoped_lock lock{mutex_};
    return read_schema_version(handle_, true);
}

common::Result<Transaction> Database::begin_transaction() {
    std::unique_lock lock{mutex_};
    if (poisoned_) {
        return common::Error{common::ErrorCode::database_error,
                             "SQLite connection is unusable after a rollback failure"};
    }
    if (transaction_active_) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "nested SQLite transactions are not supported"};
    }

    if (auto begun = internal::execute(handle_, "BEGIN IMMEDIATE", "begin SQLite transaction");
        !begun) {
        return begun.error();
    }
    transaction_active_ = true;
    return Transaction{*this, std::move(lock)};
}

common::Result<void> Database::checkpoint() {
    std::scoped_lock lock{mutex_};
    if (poisoned_) {
        return common::Error{common::ErrorCode::database_error,
                             "SQLite connection is unusable after a rollback failure"};
    }
    if (transaction_active_) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "cannot checkpoint during an active SQLite transaction"};
    }

    int log_frames = -1;
    int checkpointed_frames = -1;
    const int result = sqlite3_wal_checkpoint_v2(handle_, nullptr, SQLITE_CHECKPOINT_FULL,
                                                 &log_frames, &checkpointed_frames);
    if (result != SQLITE_OK) {
        return internal::sqlite_error(handle_, result, "checkpoint SQLite state database");
    }
    if (log_frames >= 0 && checkpointed_frames < log_frames) {
        return common::Error{common::ErrorCode::database_error,
                             "SQLite state checkpoint was incomplete"};
    }
    return common::Result<void>::success();
}

const std::string& Database::path() const noexcept { return path_; }

} // namespace minitun::storage
