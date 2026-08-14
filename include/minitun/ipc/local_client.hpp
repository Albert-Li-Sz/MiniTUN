#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include <minitun/common/result.hpp>
#include <minitun/ipc/frame.hpp>
#include <minitun/ipc/protocol.hpp>

namespace minitun::ipc {

inline constexpr std::string_view kDefaultSocketPath = "/run/minitun/minitun.sock";
inline constexpr std::chrono::milliseconds kDefaultConnectTimeout{2'000};
inline constexpr std::chrono::milliseconds kDefaultRequestTimeout{30'000};
inline constexpr std::chrono::milliseconds kMaxLocalIpcTimeout{300'000};

struct LocalClientOptions final {
    std::string socket_path{kDefaultSocketPath};
    std::chrono::milliseconds connect_timeout{kDefaultConnectTimeout};
    std::chrono::milliseconds request_timeout{kDefaultRequestTimeout};
    std::size_t max_message_size{kDefaultMaxFrameSize};
};

/// Blocking, one-request-per-connection client for MiniTun's local IPC socket.
///
/// Each request uses asynchronous socket operations internally so both connect
/// and request deadlines can cancel a stalled peer. A LocalClient may be used
/// concurrently because no per-request state is retained on the object.
class LocalClient final {
  public:
    explicit LocalClient(LocalClientOptions options = {});

    [[nodiscard]] common::Result<Response> request(const Request& request) const;

  private:
    LocalClientOptions options_;
};

} // namespace minitun::ipc
