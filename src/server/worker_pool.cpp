#include <minitun/server/worker_pool.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>

namespace minitun::server {
namespace {

[[nodiscard]] common::Result<void> validate_registration(const WorkerRegistration& registration) {
    if (!common::Id::parse(registration.client_id, common::IdKind::client) ||
        !common::Id::parse(registration.worker_id, common::IdKind::connection) ||
        registration.session_generation == 0U) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "worker registration identity is invalid");
    }
    return common::Result<void>::success();
}

void close_socket(asio::ip::tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.close(ignored);
}

} // namespace

class WorkerPool::Impl final {
  private:
    struct Entry final {
        WorkerRegistration registration;
        WorkerAssignmentHandler assignment_handler;
        WorkerRemovalHandler removal_handler;
    };

  public:
    Impl(const std::size_t max_idle_workers_per_session, const std::size_t max_total_idle_workers)
        : max_idle_workers_per_session_(max_idle_workers_per_session),
          max_total_idle_workers_(max_total_idle_workers) {}

    [[nodiscard]] common::Result<void> add(WorkerRegistration registration,
                                           WorkerAssignmentHandler assignment_handler,
                                           WorkerRemovalHandler removal_handler) {
        auto valid = validate_registration(registration);
        if (!valid) {
            return valid;
        }
        if (!assignment_handler) {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "worker assignment handler is empty");
        }
        if (entries_.contains(registration.worker_id)) {
            return common::Result<void>::failure(common::ErrorCode::already_exists,
                                                 "worker ID is already registered");
        }
        if (entries_.size() >= max_total_idle_workers_ ||
            idle_count(registration.client_id, registration.session_generation) >=
                max_idle_workers_per_session_) {
            return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                                 "idle worker limit has been reached");
        }
        std::string key = registration.worker_id;
        entries_.emplace(std::move(key),
                         Entry{std::move(registration), std::move(assignment_handler),
                               std::move(removal_handler)});
        return common::Result<void>::success();
    }

    [[nodiscard]] bool assign(const TunnelBinding& binding, asio::ip::tcp::socket& public_socket,
                              ConnectionQuota::Lease& connection_lease) noexcept {
        for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
            const auto& registration = iterator->second.registration;
            if (registration.client_id != binding.client_id ||
                registration.session_generation != binding.session_generation) {
                continue;
            }
            auto handler = std::move(iterator->second.assignment_handler);
            entries_.erase(iterator);
            try {
                handler(binding, std::move(public_socket), std::move(connection_lease));
            } catch (...) {
                close_socket(public_socket);
            }
            return true;
        }
        return false;
    }

    void remove(const std::string_view worker_id) noexcept {
        const auto iterator = entries_.find(std::string{worker_id});
        if (iterator == entries_.end()) {
            return;
        }
        invoke_removal(iterator->second);
        entries_.erase(iterator);
    }

    void remove_session(const std::string_view client_id,
                        const std::uint64_t session_generation) noexcept {
        erase_matching(client_id, session_generation, true);
    }

    void remove_client(const std::string_view client_id) noexcept {
        erase_matching(client_id, 0U, false);
    }

    void clear() noexcept {
        for (auto& [worker_id, entry] : entries_) {
            static_cast<void>(worker_id);
            invoke_removal(entry);
        }
        entries_.clear();
    }

    [[nodiscard]] std::size_t idle_count(const std::string_view client_id,
                                         const std::uint64_t session_generation) const noexcept {
        std::size_t count = 0U;
        for (const auto& [worker_id, entry] : entries_) {
            static_cast<void>(worker_id);
            if (entry.registration.client_id == client_id &&
                entry.registration.session_generation == session_generation) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  private:
    void erase_matching(const std::string_view client_id, const std::uint64_t session_generation,
                        const bool match_generation) noexcept {
        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            const auto& registration = iterator->second.registration;
            if (registration.client_id == client_id &&
                (!match_generation || registration.session_generation == session_generation)) {
                invoke_removal(iterator->second);
                iterator = entries_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    static void invoke_removal(Entry& entry) noexcept {
        if (!entry.removal_handler) {
            return;
        }
        try {
            entry.removal_handler();
        } catch (...) {
        }
    }

    std::size_t max_idle_workers_per_session_;
    std::size_t max_total_idle_workers_;
    std::unordered_map<std::string, Entry> entries_;
};

WorkerPool::WorkerPool(const std::size_t max_idle_workers_per_session,
                       const std::size_t max_total_idle_workers)
    : implementation_(
          std::make_unique<Impl>(max_idle_workers_per_session, max_total_idle_workers)) {}

WorkerPool::~WorkerPool() noexcept = default;

common::Result<void> WorkerPool::add(WorkerRegistration registration,
                                     WorkerAssignmentHandler assignment_handler,
                                     WorkerRemovalHandler removal_handler) {
    return implementation_->add(std::move(registration), std::move(assignment_handler),
                                std::move(removal_handler));
}

bool WorkerPool::assign(const TunnelBinding& binding, asio::ip::tcp::socket& public_socket,
                        ConnectionQuota::Lease& connection_lease) noexcept {
    return implementation_->assign(binding, public_socket, connection_lease);
}

void WorkerPool::remove(const std::string_view worker_id) noexcept {
    implementation_->remove(worker_id);
}

void WorkerPool::remove_session(const std::string_view client_id,
                                const std::uint64_t session_generation) noexcept {
    implementation_->remove_session(client_id, session_generation);
}

void WorkerPool::remove_client(const std::string_view client_id) noexcept {
    implementation_->remove_client(client_id);
}

void WorkerPool::clear() noexcept { implementation_->clear(); }

std::size_t WorkerPool::idle_count(const std::string_view client_id,
                                   const std::uint64_t session_generation) const noexcept {
    return implementation_->idle_count(client_id, session_generation);
}

std::size_t WorkerPool::size() const noexcept { return implementation_->size(); }

} // namespace minitun::server
