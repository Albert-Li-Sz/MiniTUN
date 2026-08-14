#include <minitun/daemon/tunnel_reconciler.hpp>

#include <limits>
#include <string>

#include <minitun/common/time.hpp>
#include <minitun/storage/state_repository.hpp>

namespace minitun::daemon {

TunnelReconciler::TunnelReconciler(storage::StateRepository& repository) noexcept
    : repository_(repository) {}

std::uint64_t TunnelReconciler::next_generation_locked(const std::string& server_id) noexcept {
    auto& generation = generations_[server_id];
    generation = generation == std::numeric_limits<std::uint64_t>::max() ? 1U : generation + 1U;
    if (generation == 0U) {
        generation = 1U;
    }
    return generation;
}

common::Result<std::uint64_t>
TunnelReconciler::begin_generation(const common::Id& server_id) {
    if (server_id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel generation requires a server ID"};
    }
    const std::scoped_lock lock{mutex_};
    const std::uint64_t generation = next_generation_locked(server_id.str());
    auto normalized = repository_.tunnels().mark_active_pending_by_server(
        server_id, std::nullopt, common::unix_milliseconds_now());
    if (!normalized) {
        return normalized.error();
    }
    return generation;
}

common::Result<void>
TunnelReconciler::end_generation(const common::Id& server_id, const std::uint64_t generation,
                                 const std::optional<common::Error>& error) {
    if (server_id.kind() != common::IdKind::server || generation == 0U) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "ending a tunnel generation requires valid identifiers"};
    }
    const std::scoped_lock lock{mutex_};
    const auto current = generations_.find(server_id.str());
    if (current == generations_.end() || current->second != generation) {
        return common::Result<void>::success();
    }
    auto normalized = repository_.tunnels().mark_active_pending_by_server(
        server_id, error, common::unix_milliseconds_now());
    if (!normalized) {
        return normalized.error();
    }
    return common::Result<void>::success();
}

common::Result<void>
TunnelReconciler::invalidate(const common::Id& server_id,
                            const std::optional<common::Error>& error) {
    if (server_id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "invalidating tunnel state requires a server ID"};
    }
    const std::scoped_lock lock{mutex_};
    static_cast<void>(next_generation_locked(server_id.str()));
    auto normalized = repository_.tunnels().mark_active_pending_by_server(
        server_id, error, common::unix_milliseconds_now());
    if (!normalized) {
        return normalized.error();
    }
    return common::Result<void>::success();
}

common::Result<bool> TunnelReconciler::transition(
    const common::Id& server_id, const common::Id& tunnel_id,
    const std::uint64_t generation, const std::uint64_t expected_revision,
    const storage::TunnelActualState state, const std::optional<common::Error>& error,
    const bool synchronized) {
    if (server_id.kind() != common::IdKind::server ||
        tunnel_id.kind() != common::IdKind::tunnel || generation == 0U ||
        expected_revision == 0U) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "tunnel transition requires valid generation identifiers"};
    }
    const std::scoped_lock lock{mutex_};
    const auto current = generations_.find(server_id.str());
    if (current == generations_.end() || current->second != generation) {
        return false;
    }
    return repository_.tunnels().update_runtime_state_if_revision(
        tunnel_id, server_id, expected_revision, state, error,
        common::unix_milliseconds_now(), synchronized);
}

bool TunnelReconciler::is_current(const common::Id& server_id,
                                  const std::uint64_t generation) const noexcept {
    if (server_id.kind() != common::IdKind::server || generation == 0U) {
        return false;
    }
    try {
        const std::scoped_lock lock{mutex_};
        const auto current = generations_.find(server_id.str());
        return current != generations_.end() && current->second == generation;
    } catch (...) {
        return false;
    }
}

} // namespace minitun::daemon
