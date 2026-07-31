#pragma once

#include <string_view>
#include <vector>

#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/storage/database.hpp>
#include <minitun/storage/models.hpp>

namespace minitun::storage {

/// Transactional CRUD for persisted public-server configuration and state.
class ServerRepository final {
  public:
    ServerRepository(Database& database, std::size_t max_records) noexcept;

    [[nodiscard]] common::Result<void> create(const ServerRecord& record);
    [[nodiscard]] common::Result<void> create(const ServerRecord& record, Transaction& transaction);

    [[nodiscard]] common::Result<ServerRecord> get_by_id(const common::Id& id) const;
    [[nodiscard]] common::Result<ServerRecord> get_by_name(std::string_view name) const;
    [[nodiscard]] common::Result<std::vector<ServerRecord>> list() const;

    [[nodiscard]] common::Result<void> update(const ServerRecord& record);
    [[nodiscard]] common::Result<void> update(const ServerRecord& record, Transaction& transaction);

    /// Marks the server and all child tunnels as removal tombstones.
    [[nodiscard]] common::Result<void> mark_removed(const common::Id& id, std::int64_t updated_at);
    [[nodiscard]] common::Result<void> mark_removed(const common::Id& id, std::int64_t updated_at,
                                                    Transaction& transaction);

    /// Physically removes a removed/disabled server tombstone after every
    /// child is also a removed/removing tombstone. Child rows then cascade.
    [[nodiscard]] common::Result<void> erase(const common::Id& id);
    [[nodiscard]] common::Result<void> erase(const common::Id& id, Transaction& transaction);

  private:
    Database& database_;
    std::size_t max_records_;
};

} // namespace minitun::storage
