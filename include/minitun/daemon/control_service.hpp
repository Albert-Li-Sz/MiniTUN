#pragma once

#include <functional>

#include <minitun/common/result.hpp>
#include <minitun/ipc/protocol.hpp>

namespace minitun::ipc {
class Dispatcher;
}

namespace minitun::storage {
class CredentialStore;
class StateRepository;
} // namespace minitun::storage

namespace minitun::daemon {

class ControlService final {
  public:
    ControlService(storage::StateRepository& repository, storage::CredentialStore& credentials,
                   std::function<void()> state_changed = {}) noexcept;

    ControlService(const ControlService&) = delete;
    ControlService& operator=(const ControlService&) = delete;

    [[nodiscard]] common::Result<void> register_handlers(ipc::Dispatcher& dispatcher);

  private:
    [[nodiscard]] common::Result<ipc::Json> daemon_status(const ipc::Request& request) const;
    [[nodiscard]] common::Result<ipc::Json> server_add(const ipc::Request& request);
    [[nodiscard]] common::Result<ipc::Json> server_login(const ipc::Request& request);
    [[nodiscard]] common::Result<ipc::Json> server_list(const ipc::Request& request) const;
    [[nodiscard]] common::Result<ipc::Json> server_inspect(const ipc::Request& request) const;
    [[nodiscard]] common::Result<ipc::Json> server_remove(const ipc::Request& request);
    [[nodiscard]] common::Result<ipc::Json> tunnel_add(const ipc::Request& request);
    [[nodiscard]] common::Result<ipc::Json> tunnel_list(const ipc::Request& request) const;
    [[nodiscard]] common::Result<ipc::Json> tunnel_inspect(const ipc::Request& request) const;
    [[nodiscard]] common::Result<ipc::Json> tunnel_remove(const ipc::Request& request);
    [[nodiscard]] common::Result<ipc::Json> status(const ipc::Request& request) const;
    void notify_state_changed() const noexcept;

    storage::StateRepository& repository_;
    storage::CredentialStore& credentials_;
    std::function<void()> state_changed_;
};

} // namespace minitun::daemon
