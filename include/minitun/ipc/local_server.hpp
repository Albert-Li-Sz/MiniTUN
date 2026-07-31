#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <minitun/common/result.hpp>
#include <minitun/ipc/frame.hpp>
#include <minitun/ipc/local_client.hpp>

namespace asio {
class io_context;
}

namespace minitun::ipc {

class Dispatcher;

inline constexpr std::size_t kDefaultMaxLocalConnections = 256;
inline constexpr std::size_t kMaxLocalConnections = 4'096;
inline constexpr std::uint32_t kDefaultSocketMode = 0660U;

struct LocalServerOptions final {
    std::string socket_path{kDefaultSocketPath};
    std::chrono::milliseconds request_timeout{kDefaultRequestTimeout};
    std::size_t max_message_size{kDefaultMaxFrameSize};
    std::size_t max_connections{kDefaultMaxLocalConnections};
    std::uint32_t socket_mode{kDefaultSocketMode};

    /// If owner_uid is set, it must match the daemon's effective user ID.
    /// Deployments must run the daemon as the intended socket owner. group_gid
    /// may name another group when the daemon has permission to apply it.
    std::optional<std::uint32_t> owner_uid;
    std::optional<std::uint32_t> group_gid;
};

/// Concurrent Unix-domain-socket server for the local IPC protocol.
///
/// The caller must keep io_context alive through this object's destruction.
/// Dispatcher is shared with all queued sessions. Connections run
/// independently while every individual session is serialized through its
/// own Asio strand. Destroy the server outside its registered method handlers;
/// destruction joins the bounded dispatcher pool.
class LocalServer final {
  public:
    LocalServer(asio::io_context& io_context, std::shared_ptr<Dispatcher> dispatcher,
                LocalServerOptions options = {});
    ~LocalServer();

    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;
    LocalServer(LocalServer&&) = delete;
    LocalServer& operator=(LocalServer&&) = delete;

    /// Validates the private, real parent directory; safely replaces a stale
    /// socket; binds; applies and verifies ownership/mode; then accepts.
    [[nodiscard]] common::Result<void> start();

    /// Stops accepting, closes all sessions, and removes only the socket node
    /// created by this server. Safe to call repeatedly and from any thread.
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::size_t active_connections() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace minitun::ipc
