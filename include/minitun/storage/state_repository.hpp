#pragma once

#include <memory>
#include <string_view>

#include <minitun/common/result.hpp>
#include <minitun/storage/database.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/server_repository.hpp>
#include <minitun/storage/tunnel_repository.hpp>

namespace minitun::storage {

/// Owns the daemon's migrated state database and typed repositories.
class StateRepository final {
  public:
    [[nodiscard]] static common::Result<std::unique_ptr<StateRepository>>
    open(std::string_view path = kDefaultDatabasePath, StorageLimits limits = {});

    ~StateRepository() = default;

    StateRepository(const StateRepository&) = delete;
    StateRepository& operator=(const StateRepository&) = delete;
    StateRepository(StateRepository&&) = delete;
    StateRepository& operator=(StateRepository&&) = delete;

    [[nodiscard]] ServerRepository& servers() noexcept;
    [[nodiscard]] const ServerRepository& servers() const noexcept;
    [[nodiscard]] TunnelRepository& tunnels() noexcept;
    [[nodiscard]] const TunnelRepository& tunnels() const noexcept;

    [[nodiscard]] common::Result<Transaction> begin_transaction();
    [[nodiscard]] common::Result<int> schema_version() const;

    /// Atomically converts stale process-local actual states into restart-safe
    /// states, resets transient metrics, and returns the validated snapshot.
    [[nodiscard]] common::Result<RecoverySnapshot> recover();

  private:
    StateRepository(std::unique_ptr<Database> database, StorageLimits limits);

    std::unique_ptr<Database> database_;
    StorageLimits limits_;
    ServerRepository servers_;
    TunnelRepository tunnels_;
};

} // namespace minitun::storage
