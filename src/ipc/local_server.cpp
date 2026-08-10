#include <minitun/ipc/local_server.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/bind_executor.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>
#include <asio/write.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <minitun/common/logging.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/ipc/dispatcher.hpp>
#include <minitun/ipc/frame.hpp>
#include <minitun/ipc/protocol.hpp>

#include "local_internal.hpp"

namespace minitun::ipc {
namespace {

void secure_erase_json(Json& value) noexcept {
    try {
        if (value.is_string()) {
            auto& text = value.get_ref<std::string&>();
            common::secure_erase_memory(text.data(), text.size());
            text.clear();
            return;
        }
        if (value.is_array() || value.is_object()) {
            for (auto& child : value) {
                secure_erase_json(child);
            }
        }
    } catch (...) {
    }
}

class ScrubbedRequest final {
  public:
    explicit ScrubbedRequest(const Request& request) : request_(request) {}
    ~ScrubbedRequest() noexcept { secure_erase_json(request_.params); }

    ScrubbedRequest(const ScrubbedRequest&) = delete;
    ScrubbedRequest& operator=(const ScrubbedRequest&) = delete;

    [[nodiscard]] const Request& get() const noexcept { return request_; }

  private:
    Request request_;
};

class RequestScrubber final {
  public:
    explicit RequestScrubber(Request& request) noexcept : request_(request) {}
    ~RequestScrubber() noexcept { secure_erase_json(request_.params); }

    RequestScrubber(const RequestScrubber&) = delete;
    RequestScrubber& operator=(const RequestScrubber&) = delete;

  private:
    Request& request_;
};

class StringScrubber final {
  public:
    explicit StringScrubber(std::string& value) noexcept : value_(value) {}
    ~StringScrubber() noexcept {
        common::secure_erase_memory(value_.data(), value_.size());
        value_.clear();
    }

    StringScrubber(const StringScrubber&) = delete;
    StringScrubber& operator=(const StringScrubber&) = delete;

  private:
    std::string& value_;
};

using LocalSocket = asio::local::stream_protocol::socket;
using LocalAcceptor = asio::local::stream_protocol::acceptor;

constexpr std::size_t kSessionReadBufferSize = 4096;
constexpr std::size_t kDispatcherThreadCount = 4;
constexpr auto kAcceptRetryDelay = std::chrono::milliseconds{100};
constexpr auto kStaleSocketProbeTimeout = std::chrono::milliseconds{100};

struct SocketIdentity final {
    dev_t device{};
    ino_t inode{};

    friend bool operator==(const SocketIdentity&, const SocketIdentity&) = default;
};

[[nodiscard]] common::Error posix_error(int error_number, std::string_view operation) {
    common::ErrorCode code = common::ErrorCode::ipc_error;
    if (error_number == EACCES || error_number == EPERM || error_number == ELOOP) {
        code = common::ErrorCode::permission_denied;
    } else if (error_number == ENOENT || error_number == ENOTDIR) {
        code = common::ErrorCode::not_found;
    } else if (error_number == EADDRINUSE || error_number == EEXIST) {
        code = common::ErrorCode::already_exists;
    }

    std::string message{operation};
    message.append(" failed: ");
    message.append(std::strerror(error_number));
    return common::Error{code, std::move(message)};
}

[[nodiscard]] std::string_view parent_path(std::string_view path) noexcept {
    const auto slash = path.rfind('/');
    return slash == 0 ? path.substr(0, 1) : path.substr(0, slash);
}

[[nodiscard]] common::Result<void> validate_directory_component(const std::string& path,
                                                                const bool final_parent) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
        return posix_error(errno, "IPC socket directory inspection");
    }
    if (!S_ISDIR(status.st_mode)) {
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             "IPC socket path must contain only real directories");
    }
    const uid_t effective_user = ::geteuid();
    if (final_parent && status.st_uid != effective_user) {
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             "IPC socket parent must be owned by the daemon user");
    }
    if (!final_parent && status.st_uid != 0 && status.st_uid != effective_user) {
        return common::Result<void>::failure(
            common::ErrorCode::permission_denied,
            "IPC socket ancestors must be owned by root or the daemon user");
    }
    const bool writable_by_others = (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    if (writable_by_others && (final_parent || (status.st_mode & S_ISVTX) == 0)) {
        return common::Result<void>::failure(
            common::ErrorCode::permission_denied,
            "IPC socket directories must not expose an unprotected writable namespace");
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> validate_parent_directory(std::string_view path) {
    const std::string_view parent = parent_path(path);
    auto root = validate_directory_component("/", parent == "/");
    if (!root || parent == "/") {
        return root;
    }

    std::string current;
    current.reserve(parent.size());
    std::size_t component_start = 1;
    while (component_start < parent.size()) {
        const auto component_end = parent.find('/', component_start);
        const auto component_length = component_end == std::string_view::npos
                                          ? parent.size() - component_start
                                          : component_end - component_start;
        current.push_back('/');
        current.append(parent.substr(component_start, component_length));
        const bool final_parent = component_end == std::string_view::npos;
        auto valid = validate_directory_component(current, final_parent);
        if (!valid) {
            return valid;
        }
        if (final_parent) {
            break;
        }
        component_start = component_end + 1U;
    }
    return common::Result<void>::success();
}

class SocketPathLock final {
  public:
    explicit SocketPathLock(const int descriptor) noexcept : descriptor_(descriptor) {}
    ~SocketPathLock() { reset(); }

    SocketPathLock(const SocketPathLock&) = delete;
    SocketPathLock& operator=(const SocketPathLock&) = delete;

    SocketPathLock(SocketPathLock&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    SocketPathLock& operator=(SocketPathLock&& other) noexcept {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

  private:
    void reset() noexcept {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
        }
    }

    int descriptor_{-1};
};

[[nodiscard]] common::Result<SocketPathLock> acquire_socket_path_lock(std::string_view path) {
    std::string lock_path{path};
    lock_path.append(".lock");
    const int descriptor =
        ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return posix_error(errno, "IPC socket lock open");
    }
    SocketPathLock lock{descriptor};

    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        return posix_error(errno, "IPC socket lock inspection");
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1) {
        return common::Result<SocketPathLock>::failure(
            common::ErrorCode::permission_denied,
            "IPC socket lock must be a daemon-owned regular file with one link");
    }
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        return posix_error(errno, "IPC socket lock permission update");
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return common::Result<SocketPathLock>::failure(
                common::ErrorCode::already_exists,
                "another IPC server owns the socket startup lock");
        }
        return posix_error(errno, "IPC socket lock acquisition");
    }
    return lock;
}

enum class SocketProbeResult : std::uint8_t {
    live,
    stale,
    disappeared,
};

[[nodiscard]] SocketProbeResult probe_existing_socket(std::string_view path) noexcept {
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return SocketProbeResult::live;
    }

    const int old_flags = ::fcntl(descriptor, F_GETFL, 0);
    if (old_flags < 0 || ::fcntl(descriptor, F_SETFL, old_flags | O_NONBLOCK) != 0) {
        static_cast<void>(::close(descriptor));
        return SocketProbeResult::live;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    const auto address_size = offsetof(sockaddr_un, sun_path) + path.size() + 1;
    address.sun_len = static_cast<std::uint8_t>(address_size);
#else
    const auto address_size = offsetof(sockaddr_un, sun_path) + path.size() + 1;
#endif

    int result = ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
                           static_cast<socklen_t>(address_size));
    int connection_error = result == 0 ? 0 : errno;

    if (result != 0 && connection_error == EINPROGRESS) {
        pollfd event{descriptor, POLLOUT, 0};
        result = ::poll(&event, 1, static_cast<int>(kStaleSocketProbeTimeout.count()));
        if (result > 0) {
            socklen_t error_size = static_cast<socklen_t>(sizeof(connection_error));
            if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &connection_error, &error_size) !=
                0) {
                connection_error = errno;
            }
        } else {
            connection_error = EAGAIN;
        }
    }

    static_cast<void>(::close(descriptor));
    if (connection_error == 0) {
        return SocketProbeResult::live;
    }
    if (connection_error == ECONNREFUSED) {
        return SocketProbeResult::stale;
    }
    if (connection_error == ENOENT) {
        return SocketProbeResult::disappeared;
    }
    return SocketProbeResult::live;
}

[[nodiscard]] SocketIdentity identity_from_stat(const struct stat& status) noexcept {
    return SocketIdentity{status.st_dev, status.st_ino};
}

[[nodiscard]] common::Result<void> remove_stale_socket(std::string_view path) {
    const std::string path_string{path};
    struct stat original{};
    if (::lstat(path_string.c_str(), &original) != 0) {
        if (errno == ENOENT) {
            return common::Result<void>::success();
        }
        return posix_error(errno, "existing IPC socket inspection");
    }
    if (!S_ISSOCK(original.st_mode)) {
        return common::Result<void>::failure(common::ErrorCode::already_exists,
                                             "refusing to replace a non-socket IPC path");
    }

    if ((original.st_mode & S_ISVTX) != 0) {
        return common::Result<void>::failure(common::ErrorCode::already_exists,
                                             "refusing to replace an unusual IPC socket");
    }

    const auto probe = probe_existing_socket(path);
    if (probe == SocketProbeResult::live) {
        return common::Result<void>::failure(common::ErrorCode::already_exists,
                                             "an IPC server is already listening");
    }
    if (probe == SocketProbeResult::disappeared) {
        return common::Result<void>::success();
    }

    const uid_t expected_owner = ::geteuid();
    if (original.st_uid != expected_owner) {
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             "refusing to remove another user's stale IPC socket");
    }

    struct stat current{};
    if (::lstat(path_string.c_str(), &current) != 0) {
        if (errno == ENOENT) {
            return common::Result<void>::success();
        }
        return posix_error(errno, "stale IPC socket reinspection");
    }
    if (!S_ISSOCK(current.st_mode) || identity_from_stat(current) != identity_from_stat(original)) {
        return common::Result<void>::failure(
            common::ErrorCode::already_exists,
            "IPC socket path changed while checking for a stale listener");
    }
    if (::unlink(path_string.c_str()) != 0) {
        return posix_error(errno, "stale IPC socket removal");
    }
    return common::Result<void>::success();
}

void remove_owned_socket(const std::string& path, SocketIdentity identity) noexcept {
    struct stat current{};
    if (::lstat(path.c_str(), &current) != 0 || !S_ISSOCK(current.st_mode) ||
        identity_from_stat(current) != identity) {
        return;
    }
    static_cast<void>(::unlink(path.c_str()));
}

[[nodiscard]] common::Result<void> validate_server_options(const LocalServerOptions& options) {
    auto path = detail::validate_socket_path(options.socket_path);
    if (!path) {
        return path;
    }
    auto limits =
        detail::validate_transport_limits(options.max_message_size, options.request_timeout);
    if (!limits) {
        return limits;
    }
    if (options.max_connections == 0 || options.max_connections > kMaxLocalConnections) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC connection limit must be between 1 and 4096");
    }
    if ((options.socket_mode & ~0777U) != 0U || (options.socket_mode & 0007U) != 0U) {
        return common::Result<void>::failure(
            common::ErrorCode::invalid_argument,
            "IPC socket mode must be a non-world-accessible permission mode");
    }
    if (options.owner_uid.has_value() &&
        *options.owner_uid > static_cast<std::uintmax_t>(std::numeric_limits<uid_t>::max())) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC socket owner ID is out of range");
    }
    if (options.group_gid.has_value() &&
        *options.group_gid > static_cast<std::uintmax_t>(std::numeric_limits<gid_t>::max())) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC socket group ID is out of range");
    }
    if (options.owner_uid.has_value() && static_cast<uid_t>(*options.owner_uid) != ::geteuid()) {
        return common::Result<void>::failure(
            common::ErrorCode::permission_denied,
            "IPC socket owner must match the daemon's effective user ID");
    }
    return validate_parent_directory(options.socket_path);
}

[[nodiscard]] common::Result<SocketIdentity> prepare_acceptor(LocalAcceptor& acceptor,
                                                              const LocalServerOptions& options) {
    auto stale = remove_stale_socket(options.socket_path);
    if (!stale) {
        return stale.error();
    }

    asio::error_code error;
    acceptor.open(asio::local::stream_protocol{}, error);
    if (error) {
        return detail::socket_error(error, "IPC acceptor open");
    }

    const asio::local::stream_protocol::endpoint endpoint{options.socket_path};
    acceptor.bind(endpoint, error);
    if (error) {
        asio::error_code ignored;
        acceptor.close(ignored);
        return detail::socket_error(error, "IPC socket bind");
    }

    struct stat bound_status{};
    if (::lstat(options.socket_path.c_str(), &bound_status) != 0 ||
        !S_ISSOCK(bound_status.st_mode)) {
        const int inspection_error = errno == 0 ? EIO : errno;
        asio::error_code ignored;
        acceptor.close(ignored);
        return posix_error(inspection_error, "bound IPC socket inspection");
    }
    const auto identity = identity_from_stat(bound_status);

    const auto cleanup_failure = [&](common::Error failure) {
        asio::error_code ignored;
        acceptor.close(ignored);
        remove_owned_socket(options.socket_path, identity);
        return common::Result<SocketIdentity>{std::move(failure)};
    };

    const uid_t owner =
        options.owner_uid.has_value() ? static_cast<uid_t>(*options.owner_uid) : ::geteuid();
    const gid_t group =
        options.group_gid.has_value() ? static_cast<gid_t>(*options.group_gid) : ::getegid();
    if (::lchown(options.socket_path.c_str(), owner, group) != 0) {
        return cleanup_failure(posix_error(errno, "IPC socket ownership update"));
    }
    if (::chmod(options.socket_path.c_str(), static_cast<mode_t>(options.socket_mode)) != 0) {
        return cleanup_failure(posix_error(errno, "IPC socket permission update"));
    }

    struct stat verified{};
    if (::lstat(options.socket_path.c_str(), &verified) != 0 || !S_ISSOCK(verified.st_mode) ||
        identity_from_stat(verified) != identity) {
        return cleanup_failure(common::Error{common::ErrorCode::permission_denied,
                                             "IPC socket changed during permission setup"});
    }
    if ((static_cast<std::uint32_t>(verified.st_mode) & 0777U) != options.socket_mode) {
        return cleanup_failure(common::Error{common::ErrorCode::permission_denied,
                                             "IPC socket mode verification failed"});
    }
    if (options.owner_uid.has_value() &&
        static_cast<std::uint32_t>(verified.st_uid) != *options.owner_uid) {
        return cleanup_failure(common::Error{common::ErrorCode::permission_denied,
                                             "IPC socket owner verification failed"});
    }
    if (options.group_gid.has_value() &&
        static_cast<std::uint32_t>(verified.st_gid) != *options.group_gid) {
        return cleanup_failure(common::Error{common::ErrorCode::permission_denied,
                                             "IPC socket group verification failed"});
    }
    if (!options.owner_uid.has_value() && verified.st_uid != ::geteuid()) {
        return cleanup_failure(common::Error{common::ErrorCode::permission_denied,
                                             "IPC socket effective owner verification failed"});
    }
    if (!options.group_gid.has_value() && verified.st_gid != ::getegid()) {
        return cleanup_failure(common::Error{common::ErrorCode::permission_denied,
                                             "IPC socket effective group verification failed"});
    }

    const auto backlog =
        static_cast<int>(std::min(options.max_connections, static_cast<std::size_t>(SOMAXCONN)));
    acceptor.listen(backlog, error);
    if (error) {
        return cleanup_failure(detail::socket_error(error, "IPC socket listen"));
    }
    return identity;
}

class BoundedDispatcherPool final : public std::enable_shared_from_this<BoundedDispatcherPool> {
  public:
    explicit BoundedDispatcherPool(std::size_t max_pending)
        : max_pending_(max_pending), pool_(kDispatcherThreadCount) {}

    [[nodiscard]] bool submit(std::function<void()> task) noexcept {
        try {
            std::scoped_lock lock{mutex_};
            if (!accepting_ || pending_ >= max_pending_) {
                return false;
            }
            ++pending_;
            try {
                auto self = shared_from_this();
                asio::post(pool_, [self, task = std::move(task)]() mutable noexcept {
                    try {
                        task();
                    } catch (...) {
                    }
                    self->release();
                });
                return true;
            } catch (...) {
                --pending_;
                return false;
            }
        } catch (...) {
            return false;
        }
    }

    void shutdown() {
        {
            std::scoped_lock lock{mutex_};
            accepting_ = false;
        }
        pool_.join();
    }

  private:
    void release() noexcept {
        try {
            std::scoped_lock lock{mutex_};
            --pending_;
        } catch (...) {
        }
    }

    const std::size_t max_pending_;
    std::mutex mutex_;
    std::size_t pending_{0};
    bool accepting_{true};
    asio::thread_pool pool_;
};

class Session final : public std::enable_shared_from_this<Session> {
  public:
    using FinishedHandler = std::function<void(Session*)>;

    Session(LocalSocket socket, std::shared_ptr<Dispatcher> dispatcher,
            std::shared_ptr<BoundedDispatcherPool> dispatcher_pool,
            const LocalServerOptions& options, FinishedHandler finished)
        : socket_(std::move(socket)), strand_(asio::make_strand(socket_.get_executor())),
          deadline_(strand_), dispatcher_(std::move(dispatcher)),
          dispatcher_pool_(std::move(dispatcher_pool)), decoder_(options.max_message_size),
          max_message_size_(options.max_message_size), request_timeout_(options.request_timeout),
          finished_(std::move(finished)) {}

    void start() noexcept {
        try {
            asio::dispatch(strand_, [self = shared_from_this()] {
                try {
                    self->read_more();
                } catch (...) {
                    self->close();
                }
            });
        } catch (...) {
            close_direct();
        }
    }

    void stop() noexcept {
        cancelled_.store(true, std::memory_order_release);
        try {
            asio::dispatch(strand_, [self = shared_from_this()] { self->close(); });
        } catch (...) {
            close_direct();
        }
    }

  private:
    void read_more() {
        if (closed_) {
            return;
        }
        if (!deadline_active_) {
            arm_deadline();
        }
        socket_.async_read_some(
            asio::buffer(read_buffer_),
            asio::bind_executor(
                strand_, [self = shared_from_this()](const auto& error, std::size_t bytes_read) {
                    self->on_read(error, bytes_read);
                }));
    }

    void on_read(const asio::error_code& error, std::size_t bytes_read) noexcept {
        try {
            if (closed_) {
                return;
            }
            if (error) {
                if (error == asio::error::eof) {
                    static_cast<void>(decoder_.finish());
                }
                close();
                return;
            }

            auto decoded =
                decoder_.feed(std::span<const std::uint8_t>{read_buffer_.data(), bytes_read});
            common::secure_erase_memory(read_buffer_.data(), bytes_read);
            if (!decoded) {
                close();
                return;
            }
            if (decoded->empty()) {
                read_more();
                return;
            }

            if (decoded->size() != 1U || decoder_.buffered_size() != 0U) {
                for (auto& payload : *decoded) {
                    common::secure_erase_memory(payload.data(), payload.size());
                }
                close();
                return;
            }
            process_request(decoded->front());
        } catch (...) {
            close();
        }
    }

    void process_request(std::string& payload) {
        const StringScrubber payload_scrubber{payload};
        if (closed_) {
            return;
        }
        if (cancelled_.load(std::memory_order_acquire)) {
            close();
            return;
        }
        auto request = parse_request(payload, max_message_size_);
        if (!request) {
            close();
            return;
        }
        const RequestScrubber parsed_request_scrubber{*request};
        auto dispatched_request = std::make_shared<ScrubbedRequest>(*request);

        const bool submitted = dispatcher_pool_->submit(
            [self = shared_from_this(), request = std::move(dispatched_request)]() noexcept {
                if (self->cancelled_.load(std::memory_order_acquire)) {
                    return;
                }
                const auto response = self->dispatcher_->dispatch(request->get());
                if (self->cancelled_.load(std::memory_order_acquire)) {
                    return;
                }
                try {
                    asio::post(self->strand_, [self, response]() mutable {
                        self->on_dispatch(response);
                    });
                } catch (...) {
                }
            });
        if (!submitted) {
            close();
        }
    }

    void on_dispatch(const Response& response) noexcept {
        try {
            if (closed_) {
                return;
            }
            if (cancelled_.load(std::memory_order_acquire)) {
                close();
                return;
            }
            auto serialized = serialize_response(response, max_message_size_);
            if (!serialized) {
                close();
                return;
            }
            auto frame = encode_frame(*serialized, max_message_size_);
            if (!frame) {
                close();
                return;
            }
            response_frame_ = std::move(*frame);
            asio::async_write(
                socket_, asio::buffer(response_frame_),
                asio::bind_executor(strand_, [self = shared_from_this()](
                                                 const auto& error, std::size_t /*bytes_written*/) {
                    self->on_write(error);
                }));
        } catch (...) {
            close();
        }
    }

    void on_write(const asio::error_code& error) noexcept {
        try {
            if (closed_) {
                return;
            }
            if (error) {
                close();
                return;
            }
            cancel_deadline();
            response_frame_.clear();
            close();
        } catch (...) {
            close();
        }
    }

    void arm_deadline() {
        deadline_active_ = true;
        deadline_.expires_after(request_timeout_);
        deadline_.async_wait(
            asio::bind_executor(strand_, [self = shared_from_this()](const auto& error) {
                if (!error && self->deadline_active_) {
                    self->close();
                }
            }));
    }

    void cancel_deadline() noexcept {
        deadline_active_ = false;
        detail::cancel_timer(deadline_);
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        cancelled_.store(true, std::memory_order_release);
        cancel_deadline();
        asio::error_code ignored;
        socket_.cancel(ignored);
        socket_.shutdown(LocalSocket::shutdown_both, ignored);
        socket_.close(ignored);
        if (finished_) {
            try {
                finished_(this);
            } catch (...) {
                // Session teardown must never escape an Asio completion handler.
            }
        }
    }

    void close_direct() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        cancelled_.store(true, std::memory_order_release);
        asio::error_code ignored;
        socket_.cancel(ignored);
        socket_.close(ignored);
        try {
            if (finished_) {
                finished_(this);
            }
        } catch (...) {
        }
    }

    LocalSocket socket_;
    asio::strand<asio::any_io_executor> strand_;
    asio::steady_timer deadline_;
    std::shared_ptr<Dispatcher> dispatcher_;
    std::shared_ptr<BoundedDispatcherPool> dispatcher_pool_;
    FrameDecoder decoder_;
    std::size_t max_message_size_;
    std::chrono::milliseconds request_timeout_;
    FinishedHandler finished_;
    std::array<std::uint8_t, kSessionReadBufferSize> read_buffer_{};
    std::vector<std::uint8_t> response_frame_;
    bool deadline_active_{false};
    bool closed_{false};
    std::atomic_bool cancelled_{false};
};

class ServerState final : public std::enable_shared_from_this<ServerState> {
  public:
    ServerState(asio::io_context& io_context, std::shared_ptr<Dispatcher> dispatcher,
                LocalServerOptions options)
        : io_context_(io_context), dispatcher_(std::move(dispatcher)), options_(std::move(options)),
          acceptor_(io_context), accept_retry_(io_context),
          accept_strand_(asio::make_strand(io_context)),
          dispatcher_pool_(std::make_shared<BoundedDispatcherPool>(options_.max_connections)) {}

    [[nodiscard]] common::Result<void> start() {
        if (!dispatcher_) {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "IPC dispatcher must not be null");
        }
        auto valid = validate_server_options(options_);
        if (!valid) {
            return valid;
        }

        bool accept_start_failed = false;
        {
            std::scoped_lock lock{mutex_};
            if (running_) {
                return common::Result<void>::failure(common::ErrorCode::already_exists,
                                                     "IPC server is already running");
            }
            if (shutting_down_) {
                return common::Result<void>::failure(common::ErrorCode::ipc_error,
                                                     "IPC server is shutting down");
            }

            auto path_lock = acquire_socket_path_lock(options_.socket_path);
            if (!path_lock) {
                return std::move(path_lock).error();
            }
            auto prepared = prepare_acceptor(acceptor_, options_);
            if (!prepared) {
                return std::move(prepared).error();
            }
            socket_lock_.emplace(std::move(path_lock).value());
            socket_identity_ = *prepared;
            running_ = true;
            ++generation_;
            try {
                begin_accept_locked(generation_);
            } catch (...) {
                running_ = false;
                ++generation_;
                asio::error_code ignored;
                acceptor_.close(ignored);
                if (socket_identity_.has_value()) {
                    remove_owned_socket(options_.socket_path, *socket_identity_);
                    socket_identity_.reset();
                }
                socket_lock_.reset();
                accept_start_failed = true;
            }
        }
        if (accept_start_failed) {
            return common::Result<void>::failure(common::ErrorCode::internal_error,
                                                 "failed to start the IPC accept operation");
        }
        return common::Result<void>::success();
    }

    void stop() noexcept {
        std::unordered_map<Session*, std::shared_ptr<Session>> sessions;
        std::optional<SocketIdentity> identity;
        {
            std::scoped_lock lock{mutex_};
            running_ = false;
            ++generation_;
            asio::error_code ignored;
            detail::cancel_timer(accept_retry_);
            acceptor_.cancel(ignored);
            acceptor_.close(ignored);
            sessions.swap(sessions_);
            identity = std::exchange(socket_identity_, std::nullopt);
            if (identity.has_value()) {
                remove_owned_socket(options_.socket_path, *identity);
            }
            socket_lock_.reset();
        }

        for (const auto& [key, session] : sessions) {
            static_cast<void>(key);
            session->stop();
        }
    }

    void shutdown() noexcept {
        {
            std::scoped_lock lock{mutex_};
            if (worker_join_started_) {
                return;
            }
            worker_join_started_ = true;
            shutting_down_ = true;
        }
        stop();
        try {
            dispatcher_pool_->shutdown();
        } catch (...) {
        }
    }

    [[nodiscard]] bool is_running() const noexcept {
        std::scoped_lock lock{mutex_};
        return running_;
    }

    [[nodiscard]] std::size_t active_connections() const noexcept {
        std::scoped_lock lock{mutex_};
        return sessions_.size();
    }

  private:
    void begin_accept_locked(std::uint64_t generation) {
        auto socket = std::make_shared<LocalSocket>(io_context_);
        acceptor_.async_accept(
            *socket, asio::bind_executor(accept_strand_, [self = shared_from_this(), socket,
                                                          generation](const auto& error) {
                try {
                    self->on_accept(socket, generation, error);
                } catch (...) {
                    self->fail_async_loop();
                }
            }));
    }

    void on_accept(const std::shared_ptr<LocalSocket>& socket, std::uint64_t generation,
                   const asio::error_code& error) {
        std::shared_ptr<Session> session;
        {
            std::scoped_lock lock{mutex_};
            if (!running_ || generation != generation_) {
                asio::error_code ignored;
                socket->close(ignored);
                return;
            }
            if (error) {
                if (error != asio::error::operation_aborted) {
                    schedule_accept_retry_locked(generation);
                }
                return;
            }

            if (sessions_.size() < options_.max_connections) {
                std::weak_ptr<ServerState> weak_state = shared_from_this();
                session =
                    std::make_shared<Session>(std::move(*socket), dispatcher_, dispatcher_pool_,
                                              options_, [weak_state](Session* finished) {
                                                  if (auto state = weak_state.lock()) {
                                                      state->session_finished(finished);
                                                  }
                                              });
                sessions_.emplace(session.get(), session);
            } else {
                asio::error_code ignored;
                socket->close(ignored);
            }
            begin_accept_locked(generation);
        }
        if (session) {
            session->start();
        }
    }

    void schedule_accept_retry_locked(std::uint64_t generation) {
        accept_retry_.expires_after(kAcceptRetryDelay);
        accept_retry_.async_wait(asio::bind_executor(
            accept_strand_, [self = shared_from_this(), generation](const auto& error) {
                if (error) {
                    return;
                }
                try {
                    std::scoped_lock lock{self->mutex_};
                    if (self->running_ && self->generation_ == generation) {
                        self->begin_accept_locked(generation);
                    }
                } catch (...) {
                    self->fail_async_loop();
                }
            }));
    }

    void session_finished(Session* session) {
        std::scoped_lock lock{mutex_};
        sessions_.erase(session);
    }

    void fail_async_loop() noexcept {
        common::log_error("local IPC accept loop stopped after an internal failure",
                          {.component = "ipc", .error_code = common::ErrorCode::internal_error});
        stop();
    }

    asio::io_context& io_context_;
    std::shared_ptr<Dispatcher> dispatcher_;
    const LocalServerOptions options_;
    mutable std::mutex mutex_;
    LocalAcceptor acceptor_;
    asio::steady_timer accept_retry_;
    asio::strand<asio::io_context::executor_type> accept_strand_;
    std::shared_ptr<BoundedDispatcherPool> dispatcher_pool_;
    std::unordered_map<Session*, std::shared_ptr<Session>> sessions_;
    std::optional<SocketPathLock> socket_lock_;
    std::optional<SocketIdentity> socket_identity_;
    std::uint64_t generation_{0};
    bool running_{false};
    bool shutting_down_{false};
    bool worker_join_started_{false};
};

} // namespace

class LocalServer::Impl final {
  public:
    Impl(asio::io_context& io_context, std::shared_ptr<Dispatcher> dispatcher,
         LocalServerOptions options)
        : state(std::make_shared<ServerState>(io_context, std::move(dispatcher),
                                              std::move(options))) {}

    std::shared_ptr<ServerState> state;
};

LocalServer::LocalServer(asio::io_context& io_context, std::shared_ptr<Dispatcher> dispatcher,
                         LocalServerOptions options)
    : impl_(std::make_unique<Impl>(io_context, std::move(dispatcher), std::move(options))) {}

LocalServer::~LocalServer() { impl_->state->shutdown(); }

common::Result<void> LocalServer::start() { return impl_->state->start(); }

void LocalServer::stop() noexcept { impl_->state->stop(); }

bool LocalServer::is_running() const noexcept { return impl_->state->is_running(); }

std::size_t LocalServer::active_connections() const noexcept {
    return impl_->state->active_connections();
}

} // namespace minitun::ipc
