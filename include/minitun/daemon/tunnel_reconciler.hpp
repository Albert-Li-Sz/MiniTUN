#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/storage/models.hpp>

namespace minitun::storage {
class StateRepository;
}

namespace minitun::daemon {

/// Serializes process-local session generations with persisted tunnel state.
///
/// Network responses are allowed to mutate a tunnel only when both the owning
/// session generation and the desired configuration revision are current.
/// This is the boundary that prevents an old session, delayed response, or
/// interrupted registration from reviving stale state.
class TunnelReconciler final {
  public:
    explicit TunnelReconciler(storage::StateRepository& repository) noexcept;

    [[nodiscard]] common::Result<std::uint64_t>
    begin_generation(const common::Id& server_id);

    [[nodiscard]] common::Result<void>
    end_generation(const common::Id& server_id, std::uint64_t generation,
                   const std::optional<common::Error>& error = std::nullopt);

    [[nodiscard]] common::Result<void>
    invalidate(const common::Id& server_id,
               const std::optional<common::Error>& error = std::nullopt);

    [[nodiscard]] common::Result<bool>
    transition(const common::Id& server_id, const common::Id& tunnel_id,
               std::uint64_t generation, std::uint64_t expected_revision,
               storage::TunnelActualState state,
               const std::optional<common::Error>& error = std::nullopt,
               bool synchronized = false);

    [[nodiscard]] bool is_current(const common::Id& server_id,
                                  std::uint64_t generation) const noexcept;

  private:
    [[nodiscard]] std::uint64_t next_generation_locked(const std::string& server_id) noexcept;

    storage::StateRepository& repository_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::uint64_t> generations_;
};

} // namespace minitun::daemon
