#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/storage/database.hpp>
#include <minitun/storage/models.hpp>

namespace minitun::storage {

/// Transactional CRUD for persisted TCP tunnel configuration and state.
class TunnelRepository final {
  public:
    TunnelRepository(Database& database, std::size_t max_records) noexcept;

    [[nodiscard]] common::Result<void> create(const TunnelRecord& record);
    [[nodiscard]] common::Result<void> create(const TunnelRecord& record, Transaction& transaction);

    [[nodiscard]] common::Result<TunnelRecord> get_by_id(const common::Id& id) const;

    /// Returns not_found for no match and invalid_argument for an ambiguous
    /// non-unique tunnel name.
    [[nodiscard]] common::Result<TunnelRecord> get_by_name(std::string_view name) const;

    [[nodiscard]] common::Result<std::vector<TunnelRecord>> list() const;
    [[nodiscard]] common::Result<std::vector<TunnelRecord>>
    list_by_server(const common::Id& server_id) const;

    [[nodiscard]] common::Result<void> update(const TunnelRecord& record);
    [[nodiscard]] common::Result<void> update(const TunnelRecord& record, Transaction& transaction);

    /// Marks every active tunnel for one server pending in one SQLite
    /// transaction. The most recent successful synchronization timestamp is
    /// preserved so diagnostics can distinguish stale state from never-synced
    /// state.
    [[nodiscard]] common::Result<std::size_t>
    mark_active_pending_by_server(const common::Id& server_id,
                                  const std::optional<common::Error>& error,
                                  std::int64_t updated_at);

    [[nodiscard]] common::Result<void> mark_removed(const common::Id& id, std::int64_t updated_at);
    [[nodiscard]] common::Result<void> mark_removed(const common::Id& id, std::int64_t updated_at,
                                                    Transaction& transaction);

    /// Physically removes a removed/removing tunnel tombstone.
    [[nodiscard]] common::Result<void> erase(const common::Id& id);
    [[nodiscard]] common::Result<void> erase(const common::Id& id, Transaction& transaction);

  private:
    Database& database_;
    std::size_t max_records_;
};

} // namespace minitun::storage
