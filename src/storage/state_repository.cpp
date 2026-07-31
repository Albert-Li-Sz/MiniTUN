#include <minitun/storage/state_repository.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include <minitun/common/error.hpp>
#include <minitun/common/time.hpp>

#include "sqlite_internal.hpp"

namespace minitun::storage {

common::Result<std::unique_ptr<StateRepository>> StateRepository::open(const std::string_view path,
                                                                       const StorageLimits limits) {
    constexpr std::size_t kMaximumCount =
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
    if (limits.max_servers == 0U || limits.max_tunnels == 0U ||
        limits.max_servers > kMaximumCount || limits.max_tunnels > kMaximumCount) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "storage limits must be positive signed-64-bit values"};
    }

    auto database = Database::open(path);
    if (!database) {
        return std::move(database).error();
    }
    return std::unique_ptr<StateRepository>{
        new StateRepository{std::move(*database), limits},
    };
}

StateRepository::StateRepository(std::unique_ptr<Database> database, const StorageLimits limits)
    : database_(std::move(database)), limits_(limits), servers_(*database_, limits_.max_servers),
      tunnels_(*database_, limits_.max_tunnels) {}

ServerRepository& StateRepository::servers() noexcept { return servers_; }

const ServerRepository& StateRepository::servers() const noexcept { return servers_; }

TunnelRepository& StateRepository::tunnels() noexcept { return tunnels_; }

const TunnelRepository& StateRepository::tunnels() const noexcept { return tunnels_; }

common::Result<Transaction> StateRepository::begin_transaction() {
    return database_->begin_transaction();
}

common::Result<int> StateRepository::schema_version() const { return database_->schema_version(); }

common::Result<RecoverySnapshot> StateRepository::recover() {
    auto transaction = database_->begin_transaction();
    if (!transaction) {
        return std::move(transaction).error();
    }
    const auto fail = [&transaction](common::Error error) -> common::Result<RecoverySnapshot> {
        transaction->mark_failed(error);
        auto rolled_back = transaction->commit();
        if (!rolled_back) {
            return rolled_back.error();
        }
        return error;
    };

    {
        auto persisted_servers = servers_.list();
        if (!persisted_servers) {
            return fail(persisted_servers.error());
        }
        auto persisted_tunnels = tunnels_.list();
        if (!persisted_tunnels) {
            return fail(persisted_tunnels.error());
        }
    }

    const std::int64_t recovered_at = common::unix_milliseconds_now();
    auto servers =
        internal::Statement::prepare(database_->handle_,
                                     "UPDATE servers SET "
                                     "actual_state = CASE "
                                     "  WHEN desired_state = 'enabled' AND credential_ref IS NULL "
                                     "    THEN 'not_authenticated' "
                                     "  WHEN desired_state = 'enabled' THEN 'disconnected' "
                                     "  ELSE 'disabled' "
                                     "END, "
                                     "reconnect_attempt = 0, latency_ms = NULL, "
                                     "updated_at = MAX(updated_at, ?1) "
                                     "WHERE actual_state <> CASE "
                                     "  WHEN desired_state = 'enabled' AND credential_ref IS NULL "
                                     "    THEN 'not_authenticated' "
                                     "  WHEN desired_state = 'enabled' THEN 'disconnected' "
                                     "  ELSE 'disabled' "
                                     "END OR reconnect_attempt <> 0 OR latency_ms IS NOT NULL",
                                     "normalize recovered server state");
    if (!servers) {
        return fail(servers.error());
    }
    if (auto bound = servers->bind_int64(1, recovered_at); !bound) {
        return fail(bound.error());
    }
    auto step = servers->step();
    if (!step || *step != internal::StepResult::done) {
        return fail(!step ? step.error()
                          : common::Error{common::ErrorCode::database_error,
                                          "server recovery update returned a row"});
    }

    auto removed_children = internal::Statement::prepare(
        database_->handle_,
        "UPDATE tunnels SET desired_state = 'removed', actual_state = 'removing', "
        "updated_at = MAX(updated_at, ?1) "
        "WHERE server_id IN (SELECT id FROM servers WHERE desired_state = 'removed') "
        "AND (desired_state <> 'removed' OR actual_state <> 'removing')",
        "propagate removed server tombstones");
    if (!removed_children) {
        return fail(removed_children.error());
    }
    if (auto bound = removed_children->bind_int64(1, recovered_at); !bound) {
        return fail(bound.error());
    }
    step = removed_children->step();
    if (!step || *step != internal::StepResult::done) {
        return fail(!step ? step.error()
                          : common::Error{common::ErrorCode::database_error,
                                          "server tombstone propagation returned a row"});
    }

    auto tunnels = internal::Statement::prepare(database_->handle_,
                                                "UPDATE tunnels SET "
                                                "actual_state = CASE desired_state "
                                                "  WHEN 'active' THEN 'pending' "
                                                "  WHEN 'disabled' THEN 'disabled' "
                                                "  ELSE 'removing' "
                                                "END, "
                                                "updated_at = MAX(updated_at, ?1) "
                                                "WHERE actual_state <> CASE desired_state "
                                                "  WHEN 'active' THEN 'pending' "
                                                "  WHEN 'disabled' THEN 'disabled' "
                                                "  ELSE 'removing' "
                                                "END",
                                                "normalize recovered tunnel state");
    if (!tunnels) {
        return fail(tunnels.error());
    }
    if (auto bound = tunnels->bind_int64(1, recovered_at); !bound) {
        return fail(bound.error());
    }
    step = tunnels->step();
    if (!step || *step != internal::StepResult::done) {
        return fail(!step ? step.error()
                          : common::Error{common::ErrorCode::database_error,
                                          "tunnel recovery update returned a row"});
    }

    auto recovered_servers = servers_.list();
    if (!recovered_servers) {
        return fail(recovered_servers.error());
    }
    auto recovered_tunnels = tunnels_.list();
    if (!recovered_tunnels) {
        return fail(recovered_tunnels.error());
    }

    RecoverySnapshot snapshot{
        .servers = std::move(*recovered_servers),
        .tunnels = std::move(*recovered_tunnels),
    };
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    return snapshot;
}

} // namespace minitun::storage
