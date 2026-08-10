#include <minitun/server/connection_quota.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>

namespace minitun::server {

class ConnectionQuota::Impl final {
  public:
    Impl(const std::size_t max_per_client, const std::size_t max_total) noexcept
        : max_per_client_(max_per_client), max_total_(max_total) {}

    [[nodiscard]] common::Result<void> acquire(const std::string_view client_id,
                                               const std::size_t max_for_client) {
        if (!common::Id::parse(client_id, common::IdKind::client)) {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "connection quota client ID is invalid");
        }
        if (max_for_client == 0U || max_for_client > max_per_client_) {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "connection quota override is invalid");
        }
        const std::scoped_lock lock{mutex_};
        const auto iterator = clients_.find(std::string{client_id});
        const std::size_t client_count = iterator == clients_.end() ? 0U : iterator->second;
        if (total_ >= max_total_ || client_count >= max_for_client) {
            return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                                 "connection quota has been reached");
        }
        ++total_;
        if (iterator == clients_.end()) {
            clients_.emplace(client_id, 1U);
        } else {
            ++iterator->second;
        }
        return common::Result<void>::success();
    }

    void release(const std::string_view client_id) noexcept {
        const std::scoped_lock lock{mutex_};
        const auto iterator = clients_.find(std::string{client_id});
        if (iterator == clients_.end() || iterator->second == 0U || total_ == 0U) {
            return;
        }
        --total_;
        --iterator->second;
        if (iterator->second == 0U) {
            clients_.erase(iterator);
        }
    }

    [[nodiscard]] std::size_t total_in_use() const noexcept {
        const std::scoped_lock lock{mutex_};
        return total_;
    }

    [[nodiscard]] std::size_t client_in_use(const std::string_view client_id) const noexcept {
        const std::scoped_lock lock{mutex_};
        const auto iterator = clients_.find(std::string{client_id});
        return iterator == clients_.end() ? 0U : iterator->second;
    }

    [[nodiscard]] std::size_t max_per_client() const noexcept { return max_per_client_; }

  private:
    std::size_t max_per_client_;
    std::size_t max_total_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::size_t> clients_;
    std::size_t total_{0U};
};

ConnectionQuota::Lease::Lease(std::shared_ptr<Impl> implementation, std::string client_id) noexcept
    : implementation_(std::move(implementation)), client_id_(std::move(client_id)) {}

ConnectionQuota::Lease::~Lease() noexcept { release(); }

ConnectionQuota::Lease::Lease(Lease&& other) noexcept
    : implementation_(std::move(other.implementation_)), client_id_(std::move(other.client_id_)) {}

ConnectionQuota::Lease& ConnectionQuota::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        implementation_ = std::move(other.implementation_);
        client_id_ = std::move(other.client_id_);
    }
    return *this;
}

ConnectionQuota::Lease::operator bool() const noexcept { return implementation_ != nullptr; }

void ConnectionQuota::Lease::release() noexcept {
    if (implementation_ == nullptr) {
        return;
    }
    implementation_->release(client_id_);
    implementation_.reset();
    client_id_.clear();
}

ConnectionQuota::ConnectionQuota(const std::size_t max_per_client, const std::size_t max_total)
    : implementation_(std::make_shared<Impl>(max_per_client, max_total)) {}

ConnectionQuota::~ConnectionQuota() noexcept = default;

common::Result<ConnectionQuota::Lease>
ConnectionQuota::try_acquire(const std::string_view client_id) {
    return try_acquire(client_id, implementation_->max_per_client());
}

common::Result<ConnectionQuota::Lease>
ConnectionQuota::try_acquire(const std::string_view client_id,
                             const std::size_t max_for_client) {
    auto acquired = implementation_->acquire(client_id, max_for_client);
    if (!acquired) {
        return common::Result<Lease>::failure(acquired.error());
    }
    return Lease{implementation_, std::string{client_id}};
}

std::size_t ConnectionQuota::total_in_use() const noexcept {
    return implementation_->total_in_use();
}

std::size_t ConnectionQuota::client_in_use(const std::string_view client_id) const noexcept {
    return implementation_->client_in_use(client_id);
}

} // namespace minitun::server
