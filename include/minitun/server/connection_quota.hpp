#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <minitun/common/result.hpp>

namespace minitun::server {

class ConnectionQuota final {
  private:
    class Impl;

  public:
    class Lease final {
      public:
        Lease() noexcept = default;
        ~Lease() noexcept;

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;

      private:
        friend class ConnectionQuota;

        Lease(std::shared_ptr<Impl> implementation, std::string client_id) noexcept;
        void release() noexcept;

        std::shared_ptr<Impl> implementation_;
        std::string client_id_;
    };

    ConnectionQuota(std::size_t max_per_client, std::size_t max_total);
    ~ConnectionQuota() noexcept;

    ConnectionQuota(const ConnectionQuota&) = delete;
    ConnectionQuota& operator=(const ConnectionQuota&) = delete;

    [[nodiscard]] common::Result<Lease> try_acquire(std::string_view client_id);
    [[nodiscard]] std::size_t total_in_use() const noexcept;
    [[nodiscard]] std::size_t client_in_use(std::string_view client_id) const noexcept;

  private:
    std::shared_ptr<Impl> implementation_;
};

} // namespace minitun::server
