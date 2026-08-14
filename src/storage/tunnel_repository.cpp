#include <minitun/storage/tunnel_repository.hpp>

#include <algorithm>
#include <cstdint>
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

constexpr std::string_view kTunnelColumns =
    "id, name, server_id, protocol, local_host, local_port, "
    "remote_host, remote_port, desired_state, actual_state, "
    "last_error_code, last_error_message, created_at, updated_at, last_synced_at, "
    "config_revision, managed_by_config, proxy_protocol";

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

} // namespace

TunnelRepository::TunnelRepository(Database& database, const std::size_t max_records) noexcept
    : database_(database), max_records_(max_records) {}

common::Result<void> TunnelRepository::create(const TunnelRecord& record) {
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

common::Result<void> TunnelRepository::create(const TunnelRecord& record,
                                              Transaction& transaction) {
    if (!transaction.belongs_to(database_) || transaction.failed()) {
        return transaction_error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction.mark_failed(error);
        return error;
    };

    auto validated = internal::validate_tunnel_record(record);
    if (!validated) {
        return fail(validated.error());
    }
    auto count = internal::query_single_int64(database_.handle_, "SELECT COUNT(*) FROM tunnels",
                                              "count persisted tunnels");
    if (!count) {
        return fail(count.error());
    }
    if (*count < 0 || static_cast<std::uint64_t>(*count) >= max_records_) {
        return fail(common::Error{common::ErrorCode::resource_exhausted,
                                  "configured tunnel storage limit reached"});
    }

    auto statement = internal::Statement::prepare(
        database_.handle_,
        "INSERT INTO tunnels("
        "id, name, server_id, protocol, local_host, local_port, "
        "remote_host, remote_port, desired_state, actual_state, "
        "last_error_code, last_error_message, created_at, updated_at, last_synced_at, "
        "config_revision, managed_by_config, proxy_protocol"
        ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, "
        "?16, ?17, ?18)",
        "insert tunnel record");
    if (!statement) {
        return fail(statement.error());
    }

    common::Result<void> bindings[]{
        statement->bind_text(1, record.id.str()),
        bind_optional_text(*statement, 2, record.name),
        statement->bind_text(3, record.server_id.str()),
        statement->bind_text(4, to_string(record.protocol)),
        statement->bind_text(5, record.local_endpoint.host()),
        statement->bind_int64(6, record.local_endpoint.port()),
        statement->bind_text(7, record.remote_endpoint.host()),
        statement->bind_int64(8, record.remote_endpoint.port()),
        statement->bind_text(9, to_string(record.desired_state)),
        statement->bind_text(10, to_string(record.actual_state)),
        bind_optional_error(*statement, 11, record.last_error_code),
        bind_optional_text(*statement, 12, record.last_error_message),
        statement->bind_int64(13, record.created_at_unix_ms),
        statement->bind_int64(14, record.updated_at_unix_ms),
        record.last_synced_at_unix_ms.has_value()
            ? statement->bind_int64(15, *record.last_synced_at_unix_ms)
            : statement->bind_null(15),
        statement->bind_int64(16, static_cast<std::int64_t>(record.config_revision)),
        statement->bind_int64(17, record.managed_by_config ? 1 : 0),
        statement->bind_int64(18, record.proxy_protocol ? 1 : 0),
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
                                  "tunnel insert unexpectedly returned a row"});
    }
    return common::Result<void>::success();
}

common::Result<TunnelRecord> TunnelRepository::get_by_id(const common::Id& id) const {
    if (id.kind() != common::IdKind::tunnel) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel lookup requires a tunnel ID"};
    }
    std::scoped_lock lock{database_.mutex_};
    if (database_.poisoned_) {
        return common::Error{common::ErrorCode::database_error, "SQLite connection is unusable"};
    }

    const std::string sql = "SELECT " + std::string{kTunnelColumns} + " FROM tunnels WHERE id = ?1";
    auto statement = internal::Statement::prepare(database_.handle_, sql, "select tunnel by ID");
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
        return common::Error{common::ErrorCode::not_found, "tunnel record was not found"};
    }
    auto record = internal::read_tunnel(statement->handle());
    if (!record) {
        return std::move(record).error();
    }
    step = statement->step();
    if (!step || *step != internal::StepResult::done) {
        return !step ? std::move(step).error()
                     : common::Error{common::ErrorCode::database_error,
                                     "tunnel ID lookup returned duplicate rows"};
    }
    return record;
}

common::Result<TunnelRecord> TunnelRepository::get_by_name(const std::string_view name) const {
    if (auto validated = internal::validate_text(name, 1U, kMaxNameBytes, "tunnel name");
        !validated) {
        return validated.error();
    }
    std::scoped_lock lock{database_.mutex_};
    if (database_.poisoned_) {
        return common::Error{common::ErrorCode::database_error, "SQLite connection is unusable"};
    }

    const std::string sql = "SELECT " + std::string{kTunnelColumns} +
                            " FROM tunnels WHERE name = ?1 ORDER BY id LIMIT 2";
    auto statement = internal::Statement::prepare(database_.handle_, sql, "select tunnel by name");
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
        return common::Error{common::ErrorCode::not_found, "tunnel record was not found"};
    }
    auto record = internal::read_tunnel(statement->handle());
    if (!record) {
        return std::move(record).error();
    }

    step = statement->step();
    if (!step) {
        return std::move(step).error();
    }
    if (*step == internal::StepResult::row) {
        auto duplicate = internal::read_tunnel(statement->handle());
        if (!duplicate) {
            return std::move(duplicate).error();
        }
        return common::Error{
            common::ErrorCode::invalid_argument,
            "tunnel name is ambiguous; use the tunnel ID or a server-qualified list",
        };
    }
    return record;
}

common::Result<std::vector<TunnelRecord>> TunnelRepository::list() const {
    std::scoped_lock lock{database_.mutex_};
    if (database_.poisoned_) {
        return common::Error{common::ErrorCode::database_error, "SQLite connection is unusable"};
    }

    const std::string sql =
        "SELECT " + std::string{kTunnelColumns} + " FROM tunnels ORDER BY created_at, id";
    auto statement = internal::Statement::prepare(database_.handle_, sql, "list tunnels");
    if (!statement) {
        return std::move(statement).error();
    }

    std::vector<TunnelRecord> records;
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
                                 "persisted tunnel count exceeds the configured limit"};
        }
        auto record = internal::read_tunnel(statement->handle());
        if (!record) {
            return std::move(record).error();
        }
        records.push_back(std::move(*record));
    }
    return records;
}

common::Result<std::vector<TunnelRecord>>
TunnelRepository::list_by_server(const common::Id& server_id) const {
    if (server_id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel list requires a server ID"};
    }
    std::scoped_lock lock{database_.mutex_};
    if (database_.poisoned_) {
        return common::Error{common::ErrorCode::database_error, "SQLite connection is unusable"};
    }

    const std::string sql = "SELECT " + std::string{kTunnelColumns} +
                            " FROM tunnels WHERE server_id = ?1 ORDER BY created_at, id";
    auto statement = internal::Statement::prepare(database_.handle_, sql, "list tunnels by server");
    if (!statement) {
        return std::move(statement).error();
    }
    if (auto bound = statement->bind_text(1, server_id.str()); !bound) {
        return bound.error();
    }

    std::vector<TunnelRecord> records;
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
                                 "persisted tunnel count exceeds the configured limit"};
        }
        auto record = internal::read_tunnel(statement->handle());
        if (!record) {
            return std::move(record).error();
        }
        records.push_back(std::move(*record));
    }
    return records;
}

common::Result<void> TunnelRepository::update(const TunnelRecord& record) {
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

common::Result<void> TunnelRepository::update(const TunnelRecord& record,
                                              Transaction& transaction) {
    if (!transaction.belongs_to(database_) || transaction.failed()) {
        return transaction_error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction.mark_failed(error);
        return error;
    };
    auto validated = internal::validate_tunnel_record(record);
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
            "tunnel creation time is immutable and update time/revision must not move backward",
        });
    }

    auto statement = internal::Statement::prepare(
        database_.handle_,
        "UPDATE tunnels SET "
        "name = ?1, server_id = ?2, protocol = ?3, local_host = ?4, local_port = ?5, "
        "remote_host = ?6, remote_port = ?7, desired_state = ?8, actual_state = ?9, "
        "last_error_code = ?10, last_error_message = ?11, updated_at = ?12, "
        "last_synced_at = ?13, config_revision = ?14, managed_by_config = ?15, "
        "proxy_protocol = ?16 "
        "WHERE id = ?17",
        "update tunnel record");
    if (!statement) {
        return fail(statement.error());
    }

    common::Result<void> bindings[]{
        bind_optional_text(*statement, 1, record.name),
        statement->bind_text(2, record.server_id.str()),
        statement->bind_text(3, to_string(record.protocol)),
        statement->bind_text(4, record.local_endpoint.host()),
        statement->bind_int64(5, record.local_endpoint.port()),
        statement->bind_text(6, record.remote_endpoint.host()),
        statement->bind_int64(7, record.remote_endpoint.port()),
        statement->bind_text(8, to_string(record.desired_state)),
        statement->bind_text(9, to_string(record.actual_state)),
        bind_optional_error(*statement, 10, record.last_error_code),
        bind_optional_text(*statement, 11, record.last_error_message),
        statement->bind_int64(12, record.updated_at_unix_ms),
        record.last_synced_at_unix_ms.has_value()
            ? statement->bind_int64(13, *record.last_synced_at_unix_ms)
            : statement->bind_null(13),
        statement->bind_int64(14, static_cast<std::int64_t>(record.config_revision)),
        statement->bind_int64(15, record.managed_by_config ? 1 : 0),
        statement->bind_int64(16, record.proxy_protocol ? 1 : 0),
        statement->bind_text(17, record.id.str()),
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
                                  "tunnel update unexpectedly returned a row"});
    }
    if (sqlite3_changes(database_.handle_) != 1) {
        return fail(common::Error{common::ErrorCode::not_found, "tunnel record was not found"});
    }
    return common::Result<void>::success();
}

common::Result<std::size_t>
TunnelRepository::mark_active_pending_by_server(const common::Id& server_id,
                                                const std::optional<common::Error>& error,
                                                const std::int64_t updated_at) {
    if (server_id.kind() != common::IdKind::server || updated_at < 0) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "batch tunnel update requires a server ID and non-negative time"};
    }
    if (error.has_value()) {
        const auto parsed = common::error_code_from_string(common::to_string(error->code()));
        if (error->code() == common::ErrorCode::ok || !parsed.has_value() ||
            *parsed != error->code()) {
            return common::Error{common::ErrorCode::invalid_argument,
                                 "batch tunnel update requires a known non-ok error code"};
        }
        if (auto validated = internal::validate_text(error->message(), 0U, kMaxErrorMessageBytes,
                                                     "last_error_message");
            !validated) {
            return validated.error();
        }
    }

    auto transaction = database_.begin_transaction();
    if (!transaction) {
        return std::move(transaction).error();
    }
    auto statement = internal::Statement::prepare(
        database_.handle_,
        "UPDATE tunnels SET actual_state = 'pending', last_error_code = ?1, "
        "last_error_message = ?2, updated_at = MAX(updated_at, ?3) "
        "WHERE server_id = ?4 AND desired_state = 'active'",
        "mark active server tunnels pending");
    if (!statement) {
        return statement.error();
    }
    common::Result<void> bindings[]{
        error.has_value() ? statement->bind_text(1, common::to_string(error->code()))
                          : statement->bind_null(1),
        error.has_value() ? statement->bind_text(2, error->message()) : statement->bind_null(2),
        statement->bind_int64(3, updated_at),
        statement->bind_text(4, server_id.str()),
    };
    for (auto& binding : bindings) {
        if (!binding) {
            return binding.error();
        }
    }
    auto step = statement->step();
    if (!step) {
        return step.error();
    }
    if (*step != internal::StepResult::done) {
        return common::Error{common::ErrorCode::database_error,
                             "batch tunnel update unexpectedly returned a row"};
    }
    const int changed = sqlite3_changes(database_.handle_);
    if (changed < 0) {
        return common::Error{common::ErrorCode::database_error,
                             "batch tunnel update returned an invalid change count"};
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    return static_cast<std::size_t>(changed);
}

common::Result<bool> TunnelRepository::update_runtime_state_if_revision(
    const common::Id& id, const common::Id& server_id, const std::uint64_t expected_revision,
    const TunnelActualState actual_state, const std::optional<common::Error>& error,
    const std::int64_t updated_at, const bool synchronized) {
    if (id.kind() != common::IdKind::tunnel || server_id.kind() != common::IdKind::server ||
        expected_revision == 0U ||
        expected_revision > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        updated_at < 0 ||
        (actual_state != TunnelActualState::pending &&
         actual_state != TunnelActualState::registering &&
         actual_state != TunnelActualState::active &&
         actual_state != TunnelActualState::failed &&
         actual_state != TunnelActualState::disabled)) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "conditional tunnel transition arguments are invalid"};
    }
    if (error.has_value()) {
        if (error->code() == common::ErrorCode::ok ||
            error->message().size() > kMaxErrorMessageBytes) {
            return common::Error{common::ErrorCode::invalid_argument,
                                 "conditional tunnel transition error is invalid"};
        }
    }

    auto transaction = database_.begin_transaction();
    if (!transaction) {
        return std::move(transaction).error();
    }
    auto statement = internal::Statement::prepare(
        database_.handle_,
        "UPDATE tunnels SET actual_state = ?1, last_error_code = ?2, "
        "last_error_message = ?3, updated_at = MAX(updated_at, ?4), "
        "last_synced_at = CASE WHEN ?5 = 1 THEN MAX(updated_at, ?4) ELSE last_synced_at END "
        "WHERE id = ?6 AND server_id = ?7 "
        "AND ((?1 = 'disabled' AND desired_state = 'disabled') OR "
        "     (?1 <> 'disabled' AND desired_state = 'active')) "
        "AND config_revision = ?8",
        "conditionally update tunnel runtime state");
    if (!statement) {
        return statement.error();
    }
    common::Result<void> bindings[]{
        statement->bind_text(1, to_string(actual_state)),
        error.has_value() ? statement->bind_text(2, common::to_string(error->code()))
                          : statement->bind_null(2),
        error.has_value() ? statement->bind_text(3, error->message()) : statement->bind_null(3),
        statement->bind_int64(4, updated_at),
        statement->bind_int64(5, synchronized ? 1 : 0),
        statement->bind_text(6, id.str()),
        statement->bind_text(7, server_id.str()),
        statement->bind_int64(8, static_cast<std::int64_t>(expected_revision)),
    };
    for (auto& binding : bindings) {
        if (!binding) {
            return binding.error();
        }
    }
    auto step = statement->step();
    if (!step) {
        return step.error();
    }
    if (*step != internal::StepResult::done) {
        return common::Error{common::ErrorCode::database_error,
                             "conditional tunnel update unexpectedly returned a row"};
    }
    const bool changed = sqlite3_changes(database_.handle_) == 1;
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    return changed;
}

common::Result<void> TunnelRepository::mark_removed(const common::Id& id,
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

common::Result<void> TunnelRepository::mark_removed(const common::Id& id,
                                                    const std::int64_t updated_at,
                                                    Transaction& transaction) {
    if (!transaction.belongs_to(database_) || transaction.failed()) {
        return transaction_error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction.mark_failed(error);
        return error;
    };
    if (id.kind() != common::IdKind::tunnel || updated_at < 0) {
        return fail(common::Error{common::ErrorCode::invalid_argument,
                                  "tunnel removal requires a tunnel ID and non-negative time"});
    }
    auto existing = get_by_id(id);
    if (!existing) {
        return fail(existing.error());
    }
    if (updated_at < existing->updated_at_unix_ms) {
        return fail(common::Error{common::ErrorCode::invalid_argument,
                                  "tunnel update time must not move backward"});
    }

    auto statement = internal::Statement::prepare(
        database_.handle_,
        "UPDATE tunnels SET desired_state = 'removed', actual_state = 'removing', "
        "updated_at = ?1 WHERE id = ?2",
        "mark tunnel removed");
    if (!statement) {
        return fail(statement.error());
    }
    if (auto bound = statement->bind_int64(1, updated_at); !bound) {
        return fail(bound.error());
    }
    if (auto bound = statement->bind_text(2, id.str()); !bound) {
        return fail(bound.error());
    }
    auto step = statement->step();
    if (!step) {
        return fail(step.error());
    }
    if (sqlite3_changes(database_.handle_) != 1) {
        return fail(common::Error{common::ErrorCode::not_found, "tunnel record was not found"});
    }
    return common::Result<void>::success();
}

common::Result<void> TunnelRepository::erase(const common::Id& id) {
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

common::Result<void> TunnelRepository::erase(const common::Id& id, Transaction& transaction) {
    if (!transaction.belongs_to(database_) || transaction.failed()) {
        return transaction_error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<void> {
        transaction.mark_failed(error);
        return error;
    };
    if (id.kind() != common::IdKind::tunnel) {
        return fail(common::Error{common::ErrorCode::invalid_argument,
                                  "tunnel erase requires a tunnel ID"});
    }
    auto existing = get_by_id(id);
    if (!existing) {
        return fail(existing.error());
    }
    if (existing->desired_state != TunnelDesiredState::removed ||
        existing->actual_state != TunnelActualState::removing) {
        return fail(common::Error{
            common::ErrorCode::invalid_argument,
            "tunnel must be a reconciled removal tombstone before physical erase",
        });
    }

    auto statement = internal::Statement::prepare(
        database_.handle_, "DELETE FROM tunnels WHERE id = ?1", "erase tunnel record");
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
        return fail(common::Error{common::ErrorCode::not_found, "tunnel record was not found"});
    }
    return common::Result<void>::success();
}

} // namespace minitun::storage
