#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <asio/ip/tcp.hpp>

#include <minitun/common/result.hpp>
#include <minitun/server/connection_quota.hpp>
#include <minitun/server/tunnel_registry.hpp>

namespace minitun::server {

struct WorkerRegistration final {
    std::string client_id;
    std::uint64_t session_generation{0U};
    std::string worker_id;

    friend bool operator==(const WorkerRegistration&, const WorkerRegistration&) = default;
};

using WorkerAssignmentHandler =
    std::function<void(TunnelBinding binding, asio::ip::tcp::socket public_socket,
                       ConnectionQuota::Lease connection_lease)>;
using WorkerRemovalHandler = std::function<void()>;

class WorkerPool final {
  public:
    WorkerPool(std::size_t max_idle_workers_per_session, std::size_t max_total_idle_workers);
    ~WorkerPool() noexcept;

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    [[nodiscard]] common::Result<void> add(WorkerRegistration registration,
                                           WorkerAssignmentHandler assignment_handler,
                                           WorkerRemovalHandler removal_handler = {});
    [[nodiscard]] bool assign(const TunnelBinding& binding, asio::ip::tcp::socket& public_socket,
                              ConnectionQuota::Lease& connection_lease) noexcept;
    void remove(std::string_view worker_id) noexcept;
    void remove_session(std::string_view client_id, std::uint64_t session_generation) noexcept;
    void remove_client(std::string_view client_id) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t idle_count(std::string_view client_id,
                                         std::uint64_t session_generation) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace minitun::server
