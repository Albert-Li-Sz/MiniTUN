#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <asio/any_io_executor.hpp>
#include <asio/ssl/context.hpp>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/result.hpp>

namespace minitun::daemon {

using LocalEndpointResolver =
    std::function<common::Result<common::Endpoint>(std::string_view tunnel_id)>;

class WorkerBudget final {
  public:
    explicit WorkerBudget(std::size_t maximum) noexcept;

    [[nodiscard]] bool try_acquire() noexcept;
    void release() noexcept;

    [[nodiscard]] std::size_t in_use() const noexcept;
    [[nodiscard]] std::size_t maximum() const noexcept;

  private:
    std::size_t maximum_;
    std::atomic<std::size_t> in_use_{0U};
};

struct WorkerPoolOptions final {
    common::Endpoint endpoint;
    std::string server_id;
    std::string client_id;
    std::uint64_t session_generation{0U};
    std::uint16_t min_idle_workers{2U};
    std::uint16_t max_idle_workers{32U};
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds handshake_timeout{10};
    // Until negotiation, let the server own Worker expiry for every supported
    // timeout (300 seconds plus a five-second client grace period).
    std::chrono::seconds idle_timeout{305};
    std::chrono::seconds relay_inactivity_timeout{300};
    std::chrono::seconds graceful_shutdown_timeout{10};
    bool insecure_skip_verify{false};
};

class WorkerPool final {
  public:
    [[nodiscard]] static common::Result<std::unique_ptr<WorkerPool>>
    create(asio::any_io_executor executor, std::shared_ptr<asio::ssl::context> tls_context,
           std::shared_ptr<WorkerBudget> idle_budget,
           std::shared_ptr<WorkerBudget> connection_budget, WorkerPoolOptions options,
           LocalEndpointResolver local_endpoint_resolver);

    ~WorkerPool() noexcept;

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    [[nodiscard]] common::Result<void> start();
    [[nodiscard]] common::Result<void> set_idle_timeout(std::chrono::seconds timeout);
    void request_workers(std::uint16_t count);
    void stop() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

  private:
    class Impl;

    explicit WorkerPool(std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace minitun::daemon
