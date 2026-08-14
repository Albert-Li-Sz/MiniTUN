#pragma once

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <asio/error.hpp>
#include <asio/error_code.hpp>

#include <sys/un.h>

#include <minitun/common/error.hpp>
#include <minitun/common/result.hpp>
#include <minitun/ipc/frame.hpp>
#include <minitun/ipc/local_client.hpp>

namespace minitun::ipc::detail {

template <typename Timer> void cancel_timer(Timer& timer) noexcept {
    try {
        static_cast<void>(timer.cancel());
    } catch (...) {
    }
}

[[nodiscard]] inline common::Result<void> validate_socket_path(std::string_view path) {
    if (path.empty()) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC socket path is empty");
    }
    if (path.front() != '/') {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC socket path must be absolute");
    }
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC socket path is too long");
    }
    if (path.find('\0') != std::string_view::npos) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC socket path contains NUL");
    }
    if (path.size() == 1 || path.back() == '/') {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC socket path has no filename");
    }

    std::size_t component_start = 1;
    while (component_start < path.size()) {
        const auto component_end = path.find('/', component_start);
        const auto length = (component_end == std::string_view::npos)
                                ? path.size() - component_start
                                : component_end - component_start;
        const auto component = path.substr(component_start, length);
        if (component.empty() || component == "." || component == "..") {
            return common::Result<void>::failure(
                common::ErrorCode::invalid_argument,
                "IPC socket path must not contain empty, dot, or parent components");
        }
        if (component_end == std::string_view::npos) {
            break;
        }
        component_start = component_end + 1;
    }
    return common::Result<void>::success();
}

[[nodiscard]] inline common::Result<void>
validate_transport_limits(std::size_t max_message_size, std::chrono::milliseconds timeout) {
    if (max_message_size == 0 || max_message_size > kDefaultMaxFrameSize) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC message limit must be between 1 byte and 1 MiB");
    }
    if (timeout <= std::chrono::milliseconds::zero() || timeout > kMaxLocalIpcTimeout) {
        return common::Result<void>::failure(
            common::ErrorCode::invalid_argument,
            "IPC timeout must be between 1 millisecond and 5 minutes");
    }
    return common::Result<void>::success();
}

[[nodiscard]] inline common::Error socket_error(const asio::error_code& error,
                                                std::string_view operation) {
    common::ErrorCode code = common::ErrorCode::ipc_error;
    if (error == asio::error::access_denied || error == asio::error::no_permission) {
        code = common::ErrorCode::permission_denied;
    } else if (error == asio::error::address_in_use) {
        code = common::ErrorCode::already_exists;
    } else if (error == asio::error::connection_refused ||
               error == asio::error::connection_aborted || error == asio::error::connection_reset ||
               error == asio::error::broken_pipe || error == asio::error::not_connected ||
               error.value() == ENOENT) {
        code = common::ErrorCode::connection_failed;
    } else if (error == asio::error::timed_out) {
        code = common::ErrorCode::connection_timeout;
    } else if (error == asio::error::no_descriptors || error == asio::error::no_buffer_space ||
               error == asio::error::no_memory) {
        code = common::ErrorCode::resource_exhausted;
    }

    std::string message{operation};
    message.append(" failed");
    if (error) {
        message.append(": ");
        message.append(error.message());
    }
    return common::Error{code, std::move(message)};
}

} // namespace minitun::ipc::detail
