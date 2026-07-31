#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include <asio/io_context.hpp>

#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>

namespace minitun::storage {
class CredentialStore;
class StateRepository;
} // namespace minitun::storage

namespace minitun::daemon {

struct ServerManagerOptions final {
    std::string ca_certificate_path;
    bool insecure_skip_verify{false};
    std::chrono::milliseconds reconcile_interval{200};
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds handshake_timeout{10};
    std::size_t max_idle_workers_per_server{32U};
    std::size_t max_total_idle_workers{128U};
};

class ServerManager final {
  public:
    [[nodiscard]] static common::Result<std::unique_ptr<ServerManager>>
    create(asio::io_context& io_context, storage::StateRepository& repository,
           storage::CredentialStore& credentials, common::Id client_id,
           ServerManagerOptions options = {});

    ~ServerManager() noexcept;

    ServerManager(const ServerManager&) = delete;
    ServerManager& operator=(const ServerManager&) = delete;
    ServerManager(ServerManager&&) = delete;
    ServerManager& operator=(ServerManager&&) = delete;

    [[nodiscard]] common::Result<void> start();
    void stop() noexcept;
    void notify_changed();

    [[nodiscard]] std::size_t session_count() const noexcept;

  private:
    class Impl;

    explicit ServerManager(std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace minitun::daemon
