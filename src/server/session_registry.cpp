#include <minitun/server/session_registry.hpp>

#include <algorithm>

#include <minitun/common/id.hpp>
#include <minitun/protocol/auth.hpp>

namespace minitun::server {

SessionRegistry::SessionRegistry(const std::size_t max_clients)
    : max_clients_(std::max<std::size_t>(max_clients, 1U)) {}

common::Result<std::uint64_t> SessionRegistry::open(const std::string_view client_id) {
    if (!common::Id::parse(client_id, common::IdKind::client)) {
        return common::Result<std::uint64_t>::failure(common::ErrorCode::invalid_argument,
                                                      "client ID is invalid");
    }
    auto generation = protocol::generate_session_generation();
    if (!generation) {
        return generation;
    }

    std::scoped_lock lock{mutex_};
    const auto existing = generations_.find(std::string{client_id});
    if (existing == generations_.end() && generations_.size() >= max_clients_) {
        return common::Result<std::uint64_t>::failure(
            common::ErrorCode::resource_exhausted,
            "server client limit has been reached");
    }
    generations_.insert_or_assign(std::string{client_id}, *generation);
    return *generation;
}

bool SessionRegistry::is_current(const std::string_view client_id,
                                 const std::uint64_t generation) const {
    std::scoped_lock lock{mutex_};
    const auto iterator = generations_.find(std::string{client_id});
    return iterator != generations_.end() && iterator->second == generation;
}

void SessionRegistry::close(const std::string_view client_id, const std::uint64_t generation) {
    std::scoped_lock lock{mutex_};
    const auto iterator = generations_.find(std::string{client_id});
    if (iterator != generations_.end() && iterator->second == generation) {
        generations_.erase(iterator);
    }
}

std::size_t SessionRegistry::size() const {
    std::scoped_lock lock{mutex_};
    return generations_.size();
}

std::size_t SessionRegistry::max_clients() const noexcept { return max_clients_; }

} // namespace minitun::server
