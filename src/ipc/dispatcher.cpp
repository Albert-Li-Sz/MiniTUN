#include <minitun/ipc/dispatcher.hpp>

#include <mutex>
#include <new>
#include <utility>

namespace minitun::ipc {

common::Result<void> Dispatcher::register_handler(std::string method, MethodHandler handler) {
    auto valid_method = validate_method_name(method);
    if (!valid_method) {
        return valid_method;
    }
    if (!handler) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC method handler must not be empty");
    }

    try {
        std::unique_lock lock{mutex_};
        const auto [iterator, inserted] = handlers_.emplace(std::move(method), std::move(handler));
        static_cast<void>(iterator);
        if (!inserted) {
            return common::Result<void>::failure(common::ErrorCode::already_exists,
                                                 "IPC method is already registered");
        }
        size_.store(handlers_.size(), std::memory_order_release);
        return common::Result<void>::success();
    } catch (const std::bad_alloc&) {
        return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                             "insufficient memory while registering IPC method");
    } catch (...) {
        return common::Result<void>::failure(common::ErrorCode::internal_error,
                                             "failed to register IPC method");
    }
}

common::Result<void> Dispatcher::unregister_handler(const std::string_view method) {
    auto valid_method = validate_method_name(method);
    if (!valid_method) {
        return valid_method;
    }

    try {
        std::unique_lock lock{mutex_};
        const auto iterator = handlers_.find(method);
        if (iterator == handlers_.end()) {
            return common::Result<void>::failure(common::ErrorCode::not_found,
                                                 "IPC method is not registered");
        }
        handlers_.erase(iterator);
        size_.store(handlers_.size(), std::memory_order_release);
        return common::Result<void>::success();
    } catch (const std::bad_alloc&) {
        return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                             "insufficient memory while unregistering IPC method");
    } catch (...) {
        return common::Result<void>::failure(common::ErrorCode::internal_error,
                                             "failed to unregister IPC method");
    }
}

Response Dispatcher::dispatch(const Request& request) const noexcept {
    try {
        if (request.version != kProtocolVersion) {
            return Response::failure(request.request_id,
                                     common::Error{common::ErrorCode::unsupported_version,
                                                   "unsupported IPC protocol version"});
        }
        auto valid_method = validate_method_name(request.method);
        if (!valid_method) {
            return Response::failure(request.request_id, valid_method.error());
        }
        if (!request.params.is_object()) {
            return Response::failure(request.request_id,
                                     common::Error{common::ErrorCode::invalid_argument,
                                                   "IPC params must be a JSON object"});
        }

        MethodHandler handler;
        {
            std::shared_lock lock{mutex_};
            const auto iterator = handlers_.find(request.method);
            if (iterator == handlers_.end()) {
                return Response::failure(
                    request.request_id,
                    common::Error{common::ErrorCode::not_found, "unknown IPC method"});
            }
            handler = iterator->second;
        }

        auto result = handler(request);
        if (!result) {
            return Response::failure(request.request_id, std::move(result).error());
        }
        Json response_result = std::move(result).value();
        if (!response_result.is_object()) {
            return Response::failure(request.request_id,
                                     common::Error{common::ErrorCode::internal_error,
                                                   "IPC method returned an invalid result"});
        }
        return Response::success(request.request_id, std::move(response_result));
    } catch (...) {
        // Never expose exception text: it may contain paths, credentials, or
        // other daemon-internal details supplied by a dependency.
        return Response::failure(
            request.request_id,
            common::Error{common::ErrorCode::internal_error, "IPC method handler failed"});
    }
}

std::size_t Dispatcher::size() const noexcept { return size_.load(std::memory_order_acquire); }

} // namespace minitun::ipc
