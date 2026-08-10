#include <minitun/storage/server_repository.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include <minitun/common/error.hpp>

#include "sqlite_internal.hpp"

namespace minitun::storage {
namespace {

constexpr std::string_view kServerColumns =
    "id, name, endpoint, credential_ref, remote_server_id, "
    "desired_state, actual_state, last_error_code, last_error_message, "
    "reconnect_attempt, latency_ms, created_at, updated_at, "
    "tls_server_name, ca_credential_ref, client_certificate_ref, "
    "client_private_key_ref, config_revision, managed_by_config";

[[nodiscard]] common::Result<void> bind_optional_text(internal::Statement& statement,
                                                      const int index,
                                                      const std::optional<std::string>& value) {
    return value.has_value() ? statement.bind_text(index, *value) : statement.bind_null(index);
}

[[nodiscard]] common::Result<void>
bind_optional_error(internal::Statement& statement, const int index,
                    const std::optional<common::ErrorCode> value) {
    return value.has_value() ? statement.bind_text(index, common::to_string(*value))
                             : statement.bind_null(index);
}

[[nodiscard]] common::Error transaction_error() {
    return common::Error{common::ErrorCode::invalid_argument,
                         "transaction is inactive, failed, or belongs to another database"};
}

[[nodiscard]] common::Result<bool>
has_non_tombstone_children(sqlite3* database, const common::Id& server_id,
                           const std::optional<std::int64_t> newer_than = std::nullopt) {
    std::string sql = "SELECT EXISTS("
                      "SELECT 1 FROM tunnels WHERE server_id = ?1 "
                      "AND (desired_state <> 'removed' OR actual_state <> 'removing')";
    if (newer_than.has_value()) {
        sql.append(" AND updated_at > ?2");
    }
    sql.push_back(')');

    auto statement = internal::Statement::prepare(database, sql, "inspect child tunnel tombstones");
    if (!statement) {
        return std::move(statement).error();
    }
    if (auto bound = statement->bind_text(1, server_id.str()); !bound) {
        return bound.error();
    }
    if (newer_than.has_value()) {
        if (auto bound = statement->bind_int64(2, *newer_than); !bound) {
            return bound.error();
        }
    }

    auto step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step != internal::StepResult::row ||
        sqlite3_column_type(statement->handle(), 0) != SQLITE_INTEGER) {
        return common::Error{common::ErrorCode::database_error,
                             "child tunnel inspection returned an invalid row"};
    }
    const bool exists = sqlite3_column_int64(statement->handle(), 0) != 0;
    step = statement->step();
    if (!step || *step != internal::StepResult::done) {
        return !step ? std::move(step).error()
                     : common::Error{common::ErrorCode::database_error,
                                     "child tunnel inspection returned extra rows"};
    }
    return exists;
}

} // namespace

ServerRepository::ServerRepository(Database& database, const std::size_t max_records) noexcept
    : database_(database), max_records_(max_records) {}

common::Result<void> ServerRepository::create(const ServerRecord& record) {
    auto transaction = database_.begin_transaction();
    if (!transaction) {
        return std::move(transaction).error();
    }
    auto created = create(record, *transaction);
    if (!created) {
        return created;
    }
    return transaction->commit();
}

common::Result<void> ServerRepository::create(const ServerRecord& record,
                                              Transaction& transaction) {
    if (!transaction.belongs_to(database_) || transaction.failed()) {
        return transaction_error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction.mark_failed(error);
        return error;
    };

    auto validated = internal::validate_server_record(record);
    if (!validated) {
        return fail(validated.error());
    }

    auto count = internal::query_single_int64(database_.handle_, "SELECT COUNT(*) FROM servers",
                                              "count persisted servers");
    if (!count) {
        return fail(count.error());
    }
    if (*count < 0 || static_cast<std::uint64_t>(*count) >= max_records_) {
        return fail(common::Error{common::ErrorCode::resource_exhausted,
                                  "configured server storage limit reached"});
    }

    auto statement = internal::Statement::prepare(
        database_.handle_,
        "INSERT INTO servers("
        "id, name, endpoint, credential_ref, remote_server_id, "
        "desired_state, actual_state, last_error_code, last_error_message, "
        "reconnect_attempt, latency_ms, created_at, updated_at, "
        "tls_server_name, ca_credential_ref, client_certificate_ref, "
        "client_private_key_ref, config_revision, managed_by_config"
        ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, "
        "?14, ?15, ?16, ?17, ?18, ?19)",
        "insert server record");
    if (!statement) {
        return fail(statement.error());
    }

    constexpr int kExpectedBindings = 19;
    common::Result<void> bindings[]{
        statement->bind_text(1, record.id.str()),
        bind_optional_text(*statement, 2, record.name),
        statement->bind_text(3, record.endpoint.to_string()),
        bind_optional_text(*statement, 4, record.credential_ref),
        bind_optional_text(*statement, 5, record.remote_server_id),
        statement->bind_text(6, to_string(record.desired_state)),
        statement->bind_text(7, to_string(record.actual_state)),
        bind_optional_error(*statement, 8, record.last_error_code),
        bind_optional_text(*statement, 9, record.last_error_message),
        statement->bind_int64(10, static_cast<std::int64_t>(record.reconnect_attempt)),
        record.latency_ms.has_value() ? statement->bind_int64(11, *record.latency_ms)
                                      : statement->bind_null(11),
        statement->bind_int64(12, record.created_at_unix_ms),
        statement->bind_int64(13, record.updated_at_unix_ms),
        bind_optional_text(*statement, 14, record.tls_server_name),
        bind_optional_text(*statement, 15, record.ca_credential_ref),
        bind_optional_text(*statement, 16, record.client_certificate_ref),
        bind_optional_text(*statement, 17, record.client_private_key_ref),
        statement->bind_int64(18, static_cast<std::int64_t>(record.config_revision)),
        statement->bind_int64(19, record.managed_by_config ? 1 : 0),
    };
    static_assert(std::size(bindings) == kExpectedBindings);
    for (auto& binding : bindings) {
        if (!binding) {
            return fail(binding.error());
        }
    }

    auto step = statement->step();
    if (!step) {
        return fail(step.error());
    }
    if (*step != internal::StepResult::done) {
        return fail(common::Error{common::ErrorCode::database_error,
                                  "server insert unexpectedly returned a row"});
    }
    return common::Result<void>::success();
}

common::Result<ServerRecord> ServerRepository::get_by_id(const common::Id& id) const {
    if (id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "server lookup requires a server ID"};
    }
    std::scoped_lock lock{database_.mutex_};
    if (database_.poisoned_) {
        return common::Error{common::ErrorCode::database_error, "SQLite connection is unusable"};
    }

    const std::string sql = "SELECT " + std::string{kServerColumns} + " FROM servers WHERE id = ?1";
    auto statement = internal::Statement::prepare(database_.handle_, sql, "select server by ID");
    if (!statement) {
        return std::move(statement).error();
    }
    if (auto bound = statement->bind_text(1, id.str()); !bound) {
        return bound.error();
    }
    auto step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step == internal::StepResult::done) {
        return common::Error{common::ErrorCode::not_found, "server record was not found"};
    }
    auto record = internal::read_server(statement->handle());
    if (!record) {
        return std::move(record).error();
    }
    step = statement->step();
    if (!step || *step != internal::StepResult::done) {
        return !step ? std::move(step).error()
                     : common::Error{common::ErrorCode::database_error,
                                     "server ID lookup returned duplicate rows"};
    }
    return record;
}

common::Result<ServerRecord> ServerRepository::get_by_name(const std::string_view name) const {
    if (auto validated = internal::validate_text(name, 1U, kMaxNameBytes, "server name");
        !validated) {
        return validated.error();
    }
    std::scoped_lock lock{database_.mutex_};
    if (database_.poisoned_) {
        return common::Error{common::ErrorCode::database_error, "SQLite connection is unusable"};
    }

    const std::string sql =
        "SELECT " + std::string{kServerColumns} + " FROM servers WHERE name = ?1";
    auto statement = internal::Statement::prepare(database_.handle_, sql, "select server by name");
    if (!statement) {
        return std::move(statement).error();
    }
    if (auto bound = statement->bind_text(1, name); !bound) {
        return bound.error();
    }
    auto step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step == internal::StepResult::done) {
        return common::Error{common::ErrorCode::not_found, "server record was not found"};
    }
    auto record = internal::read_server(statement->handle());
    if (!record) {
        return std::move(record).error();
    }
    step = statement->step();
    if (!step || *step != internal::StepResult::done) {
        return !step ? std::move(step).error()
                     : common::Error{common::ErrorCode::database_error,
                                     "server name uniqueness is corrupted"};
    }
    return record;
}

common::Result<std::vector<ServerRecord>> ServerRepository::list() const {
    std::scoped_lock lock{database_.mutex_};
    if (database_.poisoned_) {
        return common::Error{common::ErrorCode::database_error, "SQLite connection is unusable"};
    }

    const std::string sql =
        "SELECT " + std::string{kServerColumns} + " FROM servers ORDER BY created_at, id";
    auto statement = internal::Statement::prepare(database_.handle_, sql, "list servers");
    if (!statement) {
        return std::move(statement).error();
    }

    std::vector<ServerRecord> records;
    records.reserve(std::min<std::size_t>(max_records_, 64U));
    while (true) {
        auto step = statement->step();
        if (!step) {
            return std::move(step).error();
        }
        if (*step == internal::StepResult::done) {
            break;
        }
        if (records.size() >= max_records_) {
            return common::Error{common::ErrorCode::resource_exhausted,
                                 "persisted server count exceeds the configured limit"};
        }
        auto record = internal::read_server(statement->handle());
        if (!record) {
            return std::move(record).error();
        }
        records.push_back(std::move(*record));
    }
    return records;
}

common::Result<void> ServerRepository::update(const ServerRecord& record) {
    auto transaction = database_.begin_transaction();
    if (!transaction) {
        return std::move(transaction).error();
    }
    auto updated = update(record, *transaction);
    if (!updated) {
        return updated;
    }
    return transaction->commit();
}

common::Result<void> ServerRepository::update(const ServerRecord& record,
                                              Transaction& transaction) {
    if (!transaction.belongs_to(database_) || transaction.failed()) {
        return transaction_error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction.mark_failed(error);
        return error;
    };
    auto validated = internal::validate_server_record(record);
    if (!validated) {
        return fail(validated.error());
    }
    auto existing = get_by_id(record.id);
    if (!existing) {
        return fail(existing.error());
    }
    if (record.created_at_unix_ms != existing->created_at_unix_ms ||
        record.updated_at_unix_ms < existing->updated_at_unix_ms ||
        record.config_revision < existing->config_revision) {
        return fail(common::Error{
            common::ErrorCode::invalid_argument,
            "server creation time is immutable and update time/revision must not move backward",
        });
    }

    auto statement = internal::Statement::prepare(
        database_.handle_,
        "UPDATE servers SET "
        "name = ?1, endpoint = ?2, credential_ref = ?3, remote_server_id = ?4, "
        "desired_state = ?5, actual_state = ?6, last_error_code = ?7, "
        "last_error_message = ?8, reconnect_attempt = ?9, latency_ms = ?10, "
        "updated_at = ?11, tls_server_name = ?12, ca_credential_ref = ?13, "
        "client_certificate_ref = ?14, client_private_key_ref = ?15, "
        "config_revision = ?16, managed_by_config = ?17 WHERE id = ?18",
        "update server record");
    if (!statement) {
        return fail(statement.error());
    }

    common::Result<void> bindings[]{
        bind_optional_text(*statement, 1, record.name),
        statement->bind_text(2, record.endpoint.to_string()),
        bind_optional_text(*statement, 3, record.credential_ref),
        bind_optional_text(*statement, 4, record.remote_server_id),
        statement->bind_text(5, to_string(record.desired_state)),
        statement->bind_text(6, to_string(record.actual_state)),
        bind_optional_error(*statement, 7, record.last_error_code),
        bind_optional_text(*statement, 8, record.last_error_message),
        statement->bind_int64(9, static_cast<std::int64_t>(record.reconnect_attempt)),
        record.latency_ms.has_value() ? statement->bind_int64(10, *record.latency_ms)
                                      : statement->bind_null(10),
        statement->bind_int64(11, record.updated_at_unix_ms),
        bind_optional_text(*statement, 12, record.tls_server_name),
        bind_optional_text(*statement, 13, record.ca_credential_ref),
        bind_optional_text(*statement, 14, record.client_certificate_ref),
        bind_optional_text(*statement, 15, record.client_private_key_ref),
        statement->bind_int64(16, static_cast<std::int64_t>(record.config_revision)),
        statement->bind_int64(17, record.managed_by_config ? 1 : 0),
        statement->bind_text(18, record.id.str()),
    };
    for (auto& binding : bindings) {
        if (!binding) {
            return fail(binding.error());
        }
    }
    auto step = statement->step();
    if (!step) {
        return fail(step.error());
    }
    if (*step != internal::StepResult::done) {
        return fail(common::Error{common::ErrorCode::database_error,
                                  "server update unexpectedly returned a row"});
    }
    if (sqlite3_changes(database_.handle_) != 1) {
        return fail(common::Error{common::ErrorCode::not_found, "server record was not found"});
    }
    return common::Result<void>::success();
}

common::Result<void> ServerRepository::mark_removed(const common::Id& id,
                                                    const std::int64_t updated_at) {
    auto transaction = database_.begin_transaction();
    if (!transaction) {
        return std::move(transaction).error();
    }
    auto marked = mark_removed(id, updated_at, *transaction);
    if (!marked) {
        return marked;
    }
    return transaction->commit();
}

common::Result<void> ServerRepository::mark_removed(const common::Id& id,
                                                    const std::int64_t updated_at,
                                                    Transaction& transaction) {
    if (!transaction.belongs_to(database_) || transaction.failed()) {
        return transaction_error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction.mark_failed(error);
        return error;
    };
    if (id.kind() != common::IdKind::server || updated_at < 0) {
        return fail(common::Error{common::ErrorCode::invalid_argument,
                                  "server removal requires a server ID and non-negative time"});
    }
    auto existing = get_by_id(id);
    if (!existing) {
        return fail(existing.error());
    }
    if (updated_at < existing->updated_at_unix_ms) {
        return fail(common::Error{common::ErrorCode::invalid_argument,
                                  "server update time must not move backward"});
    }
    auto newer_children = has_non_tombstone_children(database_.handle_, id, updated_at);
    if (!newer_children) {
        return fail(newer_children.error());
    }
    if (*newer_children) {
        return fail(common::Error{
            common::ErrorCode::invalid_argument,
            "server removal time precedes a child tunnel update",
        });
    }

    auto children = internal::Statement::prepare(
        database_.handle_,
        "UPDATE tunnels SET desired_state = 'removed', actual_state = 'removing', "
        "updated_at = ?1 WHERE server_id = ?2 AND "
        "(desired_state <> 'removed' OR actual_state <> 'removing')",
        "mark child tunnels removed");
    if (!children) {
        return fail(children.error());
    }
    if (auto bound = children->bind_int64(1, updated_at); !bound) {
        return fail(bound.error());
    }
    if (auto bound = children->bind_text(2, id.str()); !bound) {
        return fail(bound.error());
    }
    auto step = children->step();
    if (!step) {
        return fail(step.error());
    }

    auto server = internal::Statement::prepare(
        database_.handle_,
        "UPDATE servers SET desired_state = 'removed', actual_state = 'disabled', "
        "reconnect_attempt = 0, latency_ms = NULL, updated_at = ?1 WHERE id = ?2",
        "mark server removed");
    if (!server) {
        return fail(server.error());
    }
    if (auto bound = server->bind_int64(1, updated_at); !bound) {
        return fail(bound.error());
    }
    if (auto bound = server->bind_text(2, id.str()); !bound) {
        return fail(bound.error());
    }
    step = server->step();
    if (!step) {
        return fail(step.error());
    }
    if (sqlite3_changes(database_.handle_) != 1) {
        return fail(common::Error{common::ErrorCode::not_found, "server record was not found"});
    }
    return common::Result<void>::success();
}

common::Result<void> ServerRepository::erase(const common::Id& id) {
    auto transaction = database_.begin_transaction();
    if (!transaction) {
        return std::move(transaction).error();
    }
    auto erased = erase(id, *transaction);
    if (!erased) {
        return erased;
    }
    return transaction->commit();
}

common::Result<void> ServerRepository::erase(const common::Id& id, Transaction& transaction) {
    if (!transaction.belongs_to(database_) || transaction.failed()) {
        return transaction_error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction.mark_failed(error);
        return error;
    };
    if (id.kind() != common::IdKind::server) {
        return fail(common::Error{common::ErrorCode::invalid_argument,
                                  "server erase requires a server ID"});
    }
    auto existing = get_by_id(id);
    if (!existing) {
        return fail(existing.error());
    }
    if (existing->desired_state != ServerDesiredState::removed ||
        existing->actual_state != ServerActualState::disabled) {
        return fail(common::Error{
            common::ErrorCode::invalid_argument,
            "server must be a reconciled removal tombstone before physical erase",
        });
    }
    auto live_children = has_non_tombstone_children(database_.handle_, id);
    if (!live_children) {
        return fail(live_children.error());
    }
    if (*live_children) {
        return fail(common::Error{
            common::ErrorCode::invalid_argument,
            "server still has child tunnels that are not removal tombstones",
        });
    }

    auto statement = internal::Statement::prepare(
        database_.handle_, "DELETE FROM servers WHERE id = ?1", "erase server record");
    if (!statement) {
        return fail(statement.error());
    }
    if (auto bound = statement->bind_text(1, id.str()); !bound) {
        return fail(bound.error());
    }
    auto step = statement->step();
    if (!step) {
        return fail(step.error());
    }
    if (sqlite3_changes(database_.handle_) != 1) {
        return fail(common::Error{common::ErrorCode::not_found, "server record was not found"});
    }
    return common::Result<void>::success();
}

} // namespace minitun::storage
