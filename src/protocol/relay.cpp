#include <minitun/protocol/relay.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <minitun/common/error.hpp>

namespace minitun::protocol {
namespace {

inline constexpr std::chrono::hours kMaximumRelayTimeout{24};

[[nodiscard]] bool is_end_of_stream(const asio::error_code& error) noexcept {
    return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
}

[[nodiscard]] bool is_normal_disconnect(const asio::error_code& error) noexcept {
    return is_end_of_stream(error) || error == asio::error::connection_reset ||
           error == asio::error::broken_pipe || error == asio::error::operation_aborted;
}

void close_tcp_socket(asio::ip::tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

void shutdown_send(asio::ip::tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

void shutdown_send(TlsStream& stream) noexcept {
    asio::error_code ignored;
    stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

class RelayOperation final : public std::enable_shared_from_this<RelayOperation> {
  public:
    RelayOperation(TlsStream& tls_stream, asio::ip::tcp::socket& tcp_socket,
                   const std::chrono::seconds inactivity_timeout)
        : tls_stream_(tls_stream), tcp_socket_(tcp_socket), inactivity_timeout_(inactivity_timeout),
          activity_timer_(tls_stream.get_executor()), completion_timer_(tls_stream.get_executor()),
          started_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] asio::awaitable<common::Result<RelayStats>> run() {
        activity_timer_.expires_after(inactivity_timeout_);
        completion_timer_.expires_at(std::chrono::steady_clock::time_point::max());
        auto self = shared_from_this();
        asio::co_spawn(tls_stream_.get_executor(), pump_tls_to_tcp(),
                       [self](const std::exception_ptr& failure) {
                           if (failure) {
                               self->finish_direction(
                                   common::Error{common::ErrorCode::internal_error,
                                                 "TLS-to-TCP relay direction failed unexpectedly"});
                           }
                       });
        asio::co_spawn(tls_stream_.get_executor(), pump_tcp_to_tls(),
                       [self](const std::exception_ptr& failure) {
                           if (failure) {
                               self->finish_direction(
                                   common::Error{common::ErrorCode::internal_error,
                                                 "TCP-to-TLS relay direction failed unexpectedly"});
                           }
                       });
        asio::co_spawn(tls_stream_.get_executor(), watch_inactivity(),
                       [self](const std::exception_ptr& failure) {
                           if (failure) {
                               self->set_failure(
                                   common::Error{common::ErrorCode::internal_error,
                                                 "relay inactivity monitor failed unexpectedly"});
                               self->close_both();
                           }
                       });

        asio::error_code wait_error;
        co_await completion_timer_.async_wait(
            asio::redirect_error(asio::use_awaitable, wait_error));
        done_ = true;
        cancel_timer(activity_timer_);
        stats_.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_);
        if (failure_.has_value()) {
            co_return common::Result<RelayStats>::failure(std::move(*failure_));
        }
        co_return stats_;
    }

  private:
    [[nodiscard]] asio::awaitable<void> pump_tls_to_tcp() {
        for (;;) {
            asio::error_code read_error;
            const std::size_t bytes = co_await tls_stream_.async_read_some(
                asio::buffer(tls_to_tcp_buffer_),
                asio::redirect_error(asio::use_awaitable, read_error));
            if (bytes > 0U) {
                asio::error_code write_error;
                const std::size_t written = co_await asio::async_write(
                    tcp_socket_, asio::buffer(tls_to_tcp_buffer_.data(), bytes),
                    asio::redirect_error(asio::use_awaitable, write_error));
                if (write_error || written != bytes) {
                    finish_write_error(write_error, "local TCP relay write failed");
                    co_return;
                }
                stats_.tls_to_tcp_bytes += static_cast<std::uint64_t>(written);
                note_activity();
            }
            if (read_error) {
                if (is_end_of_stream(read_error)) {
                    shutdown_send(tcp_socket_);
                    finish_direction();
                } else if (is_normal_disconnect(read_error)) {
                    finish_direction_and_close();
                } else {
                    finish_direction(common::Error{common::ErrorCode::connection_failed,
                                                   "TLS relay read failed"});
                }
                co_return;
            }
        }
    }

    [[nodiscard]] asio::awaitable<void> pump_tcp_to_tls() {
        for (;;) {
            asio::error_code read_error;
            const std::size_t bytes = co_await tcp_socket_.async_read_some(
                asio::buffer(tcp_to_tls_buffer_),
                asio::redirect_error(asio::use_awaitable, read_error));
            if (bytes > 0U) {
                asio::error_code write_error;
                const std::size_t written = co_await asio::async_write(
                    tls_stream_, asio::buffer(tcp_to_tls_buffer_.data(), bytes),
                    asio::redirect_error(asio::use_awaitable, write_error));
                if (write_error || written != bytes) {
                    finish_write_error(write_error, "TLS relay write failed");
                    co_return;
                }
                stats_.tcp_to_tls_bytes += static_cast<std::uint64_t>(written);
                note_activity();
            }
            if (read_error) {
                if (is_end_of_stream(read_error)) {
                    shutdown_send(tls_stream_);
                    finish_direction();
                } else if (is_normal_disconnect(read_error)) {
                    finish_direction_and_close();
                } else {
                    finish_direction(common::Error{common::ErrorCode::connection_failed,
                                                   "local TCP relay read failed"});
                }
                co_return;
            }
        }
    }

    [[nodiscard]] asio::awaitable<void> watch_inactivity() {
        while (!done_) {
            asio::error_code error;
            co_await activity_timer_.async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (done_) {
                co_return;
            }
            if (error == asio::error::operation_aborted) {
                continue;
            }
            if (!error) {
                set_failure(common::Error{common::ErrorCode::connection_timeout,
                                          "TCP relay inactivity timeout expired"});
            } else {
                set_failure(common::Error{common::ErrorCode::internal_error,
                                          "TCP relay inactivity timer failed"});
            }
            close_both();
            co_return;
        }
    }

    void finish_write_error(const asio::error_code& error, const char* message) {
        if (is_normal_disconnect(error)) {
            finish_direction_and_close();
            return;
        }
        finish_direction(common::Error{common::ErrorCode::connection_failed, std::string{message}});
    }

    void finish_direction(std::optional<common::Error> failure = std::nullopt) {
        if (done_) {
            return;
        }
        if (failure.has_value()) {
            set_failure(std::move(*failure));
            close_both();
        }
        ++finished_directions_;
        if (finished_directions_ >= 2U) {
            done_ = true;
            cancel_timer(activity_timer_);
            cancel_timer(completion_timer_);
        }
    }

    void finish_direction_and_close() {
        close_both();
        finish_direction();
    }

    void set_failure(common::Error failure) {
        if (!failure_.has_value()) {
            failure_ = std::move(failure);
        }
    }

    void note_activity() {
        if (!done_) {
            activity_timer_.expires_after(inactivity_timeout_);
        }
    }

    void close_both() noexcept {
        close_tls_stream(tls_stream_);
        close_tcp_socket(tcp_socket_);
    }

    static void cancel_timer(asio::steady_timer& timer) noexcept {
        try {
            static_cast<void>(timer.cancel());
        } catch (...) {
        }
    }

    TlsStream& tls_stream_;
    asio::ip::tcp::socket& tcp_socket_;
    std::chrono::seconds inactivity_timeout_;
    asio::steady_timer activity_timer_;
    asio::steady_timer completion_timer_;
    std::array<std::uint8_t, kRelayBufferSize> tls_to_tcp_buffer_{};
    std::array<std::uint8_t, kRelayBufferSize> tcp_to_tls_buffer_{};
    RelayStats stats_;
    std::optional<common::Error> failure_;
    std::chrono::steady_clock::time_point started_;
    std::size_t finished_directions_{0U};
    bool done_{false};
};

} // namespace

asio::awaitable<common::Result<RelayStats>> relay_tls_and_tcp(TlsStream& tls_stream,
                                                              asio::ip::tcp::socket& tcp_socket,
                                                              const RelayOptions options) {
    if (options.inactivity_timeout <= std::chrono::seconds::zero() ||
        options.inactivity_timeout > kMaximumRelayTimeout) {
        co_return common::Result<RelayStats>::failure(common::ErrorCode::invalid_argument,
                                                      "TCP relay timeout is invalid");
    }
    auto operation =
        std::make_shared<RelayOperation>(tls_stream, tcp_socket, options.inactivity_timeout);
    co_return co_await operation->run();
}

} // namespace minitun::protocol
