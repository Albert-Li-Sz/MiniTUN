#include <minitun/server/accept_recovery.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <asio/error.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace minitun::server {
namespace {

inline constexpr auto kInitialRetryDelay = std::chrono::milliseconds{10};
inline constexpr auto kMaximumRetryDelay = std::chrono::seconds{1};
inline constexpr auto kLogInterval = std::chrono::seconds{5};

} // namespace

std::chrono::milliseconds AcceptRetryPolicy::next_delay() noexcept {
    constexpr std::size_t kMaximumShift = 7U;
    const std::size_t shift = std::min(failure_count_, kMaximumShift);
    if (failure_count_ < kMaximumShift) {
        ++failure_count_;
    }
    return std::min(kInitialRetryDelay * static_cast<std::int64_t>(1ULL << shift),
                    std::chrono::duration_cast<std::chrono::milliseconds>(kMaximumRetryDelay));
}

void AcceptRetryPolicy::reset() noexcept {
    failure_count_ = 0U;
    logged_ = false;
    last_log_ = {};
}

bool AcceptRetryPolicy::should_log(const Clock::time_point now) noexcept {
    if (!logged_ || now - last_log_ >= kLogInterval) {
        logged_ = true;
        last_log_ = now;
        return true;
    }
    return false;
}

bool AcceptRetryPolicy::descriptor_exhausted(const asio::error_code& error) noexcept {
    return error == asio::error::no_descriptors || error.value() == ENFILE;
}

bool AcceptRetryPolicy::resource_exhausted(const asio::error_code& error) noexcept {
    return descriptor_exhausted(error) || error == asio::error::no_buffer_space ||
           error == asio::error::no_memory;
}

bool AcceptRetryPolicy::retryable(const asio::error_code& error) noexcept {
    return error && error != asio::error::operation_aborted && error != asio::error::bad_descriptor;
}

ReservedFileDescriptor::ReservedFileDescriptor() noexcept { reopen(); }

ReservedFileDescriptor::~ReservedFileDescriptor() noexcept { close(); }

void ReservedFileDescriptor::reopen() noexcept {
    if (descriptor_ >= 0) {
        return;
    }
    descriptor_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

void ReservedFileDescriptor::close() noexcept {
    if (descriptor_ < 0) {
        return;
    }
    static_cast<void>(::close(descriptor_));
    descriptor_ = -1;
}

void ReservedFileDescriptor::recover(asio::ip::tcp::acceptor& acceptor) noexcept {
    close();
    const int listener = acceptor.native_handle();
    const int original_flags = ::fcntl(listener, F_GETFL, 0);
    if (original_flags >= 0 && (original_flags & O_NONBLOCK) == 0) {
        static_cast<void>(::fcntl(listener, F_SETFL, original_flags | O_NONBLOCK));
    }

    int dropped = -1;
    do {
        dropped = ::accept(listener, nullptr, nullptr);
    } while (dropped < 0 && errno == EINTR);
    if (dropped >= 0) {
        static_cast<void>(::close(dropped));
    }
    if (original_flags >= 0 && (original_flags & O_NONBLOCK) == 0) {
        static_cast<void>(::fcntl(listener, F_SETFL, original_flags));
    }
    reopen();
}

bool ReservedFileDescriptor::available() const noexcept { return descriptor_ >= 0; }

} // namespace minitun::server
