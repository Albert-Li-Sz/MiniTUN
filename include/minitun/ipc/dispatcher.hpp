#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>

#include <minitun/common/result.hpp>
#include <minitun/ipc/protocol.hpp>

namespace minitun::ipc {

using MethodHandler = std::function<common::Result<Json>(const Request& request)>;

/// Thread-safe registry and dispatcher for local IPC methods.
///
/// Registry locks are never held while a handler runs. This permits concurrent
/// requests and allows a handler to register or remove methods without a
/// self-deadlock. Handler exceptions are converted to a generic internal_error
/// response and their exception text is never exposed to the peer.
class Dispatcher final {
  public:
    Dispatcher() = default;
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;

    [[nodiscard]] common::Result<void> register_handler(std::string method, MethodHandler handler);
    [[nodiscard]] common::Result<void> unregister_handler(std::string_view method);

    [[nodiscard]] Response dispatch(const Request& request) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    mutable std::shared_mutex mutex_;
    std::map<std::string, MethodHandler, std::less<>> handlers_;
    std::atomic_size_t size_{0};
};

} // namespace minitun::ipc
