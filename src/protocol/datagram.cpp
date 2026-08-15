#include <minitun/protocol/datagram.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <minitun/common/error.hpp>

namespace minitun::protocol {
namespace {

inline constexpr std::chrono::hours kMaximumRelayTimeout{24};

[[nodiscard]] bool is_graceful_eof(const asio::error_code& error) noexcept {
    return error == asio::error::eof || error == asio::ssl::error::stream_truncated ||
           error == asio::error::connection_reset || error == asio::error::broken_pipe ||
           error == asio::error::operation_aborted;
}

void close_udp(asio::ip::udp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.close(ignored);
}

template <typename Stream>
void close_stream(Stream& stream) noexcept {
    try {
        asio::error_code ignored;
        stream.lowest_layer().cancel(ignored);
        stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        stream.lowest_layer().close(ignored);
    } catch (...) {
    }
}

template <typename Stream>
class DatagramRelayOperation final
    : public std::enable_shared_from_this<DatagramRelayOperation<Stream>> {
  public:
    DatagramRelayOperation(Stream& stream, asio::ip::udp::socket& udp_socket,
                           const std::chrono::seconds inactivity_timeout)
        : stream_(stream), udp_socket_(udp_socket), inactivity_timeout_(inactivity_timeout),
          activity_timer_(stream.get_executor()), completion_timer_(stream.get_executor()),
          started_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] asio::awaitable<common::Result<DatagramRelayStats>> run() {
        activity_timer_.expires_after(inactivity_timeout_);
        completion_timer_.expires_at(std::chrono::steady_clock::time_point::max());
        auto self = this->shared_from_this();
        asio::co_spawn(stream_.get_executor(), pump_stream_to_udp(),
                       [self](const std::exception_ptr& failure) {
                           if (failure) {
                               self->fail(common::ErrorCode::internal_error,
                                          "stream-to-UDP relay failed unexpectedly");
                           }
                       });
        asio::co_spawn(stream_.get_executor(), pump_udp_to_stream(),
                       [self](const std::exception_ptr& failure) {
                           if (failure) {
                               self->fail(common::ErrorCode::internal_error,
                                          "UDP-to-stream relay failed unexpectedly");
                           }
                       });
        asio::co_spawn(stream_.get_executor(), watch_inactivity(),
                       [self](const std::exception_ptr& failure) {
                           if (failure) {
                               self->fail(common::ErrorCode::internal_error,
                                          "UDP relay inactivity monitor failed unexpectedly");
                           }
                       });

        asio::error_code ignored;
        co_await completion_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        done_ = true;
        cancel(activity_timer_);
        stats_.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_);
        if (failure_.has_value()) {
            co_return common::Result<DatagramRelayStats>::failure(std::move(*failure_));
        }
        co_return stats_;
    }

  private:
    [[nodiscard]] asio::awaitable<void> pump_stream_to_udp() {
        for (;;) {
            std::array<std::uint8_t, kDatagramRecordHeaderSize> header{};
            asio::error_code error;
            const std::size_t header_read =
                co_await asio::async_read(stream_, asio::buffer(header),
                                          asio::redirect_error(asio::use_awaitable, error));
            if (error || header_read != header.size()) {
                finish_transport(error, "UDP relay record header read failed");
                co_return;
            }
            const std::size_t length =
                (static_cast<std::size_t>(header[0]) << 8U) | static_cast<std::size_t>(header[1]);
            std::array<std::uint8_t, kMaximumUdpPayloadSize> payload{};
            if (length != 0U) {
                const std::size_t payload_read =
                    co_await asio::async_read(stream_, asio::buffer(payload.data(), length),
                                              asio::redirect_error(asio::use_awaitable, error));
                if (error || payload_read != length) {
                    finish_transport(error, "UDP relay record payload read failed");
                    co_return;
                }
            }
            const std::size_t sent =
                co_await udp_socket_.async_send(asio::buffer(payload.data(), length),
                                                asio::redirect_error(asio::use_awaitable, error));
            if (error || sent != length) {
                finish_transport(error, "local UDP send failed");
                co_return;
            }
            stats_.tls_to_udp_bytes += static_cast<std::uint64_t>(sent);
            ++stats_.tls_to_udp_datagrams;
            note_activity();
        }
    }

    [[nodiscard]] asio::awaitable<void> pump_udp_to_stream() {
        std::array<std::uint8_t, kMaximumUdpPayloadSize> payload{};
        for (;;) {
            asio::error_code error;
            const std::size_t received = co_await udp_socket_.async_receive(
                asio::buffer(payload), asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                finish_transport(error, "local UDP receive failed");
                co_return;
            }
            const std::array<std::uint8_t, kDatagramRecordHeaderSize> header{
                static_cast<std::uint8_t>((received >> 8U) & 0xffU),
                static_cast<std::uint8_t>(received & 0xffU)};
            const std::array<asio::const_buffer, 2U> buffers{
                asio::buffer(header), asio::buffer(payload.data(), received)};
            const std::size_t written = co_await asio::async_write(
                stream_, buffers, asio::redirect_error(asio::use_awaitable, error));
            if (error || written != received + header.size()) {
                finish_transport(error, "UDP relay record write failed");
                co_return;
            }
            stats_.udp_to_tls_bytes += static_cast<std::uint64_t>(received);
            ++stats_.udp_to_tls_datagrams;
            note_activity();
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
            fail(error ? common::ErrorCode::internal_error : common::ErrorCode::connection_timeout,
                 error ? "UDP relay inactivity timer failed"
                       : "UDP relay inactivity timeout expired");
            co_return;
        }
    }

    void finish_transport(const asio::error_code& error, const char* message) {
        if (!error || is_graceful_eof(error)) {
            finish();
            return;
        }
        fail(common::ErrorCode::connection_failed, message);
    }

    void fail(const common::ErrorCode code, const char* message) {
        if (!failure_.has_value()) {
            failure_.emplace(code, message);
        }
        finish();
    }

    void finish() noexcept {
        if (done_) {
            return;
        }
        done_ = true;
        close_stream(stream_);
        close_udp(udp_socket_);
        cancel(activity_timer_);
        cancel(completion_timer_);
    }

    void note_activity() {
        if (!done_) {
            activity_timer_.expires_after(inactivity_timeout_);
        }
    }

    static void cancel(asio::steady_timer& timer) noexcept {
        try {
            static_cast<void>(timer.cancel());
        } catch (...) {
        }
    }

    Stream& stream_;
    asio::ip::udp::socket& udp_socket_;
    std::chrono::seconds inactivity_timeout_;
    asio::steady_timer activity_timer_;
    asio::steady_timer completion_timer_;
    DatagramRelayStats stats_;
    std::optional<common::Error> failure_;
    std::chrono::steady_clock::time_point started_;
    bool done_{false};
};

} // namespace

common::Result<std::vector<std::uint8_t>>
encode_datagram_record(const std::span<const std::uint8_t> payload) {
    if (payload.size() > kMaximumUdpPayloadSize) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::invalid_argument, "UDP payload exceeds the protocol limit");
    }
    try {
        std::vector<std::uint8_t> record;
        record.reserve(kDatagramRecordHeaderSize + payload.size());
        record.push_back(static_cast<std::uint8_t>((payload.size() >> 8U) & 0xffU));
        record.push_back(static_cast<std::uint8_t>(payload.size() & 0xffU));
        record.insert(record.end(), payload.begin(), payload.end());
        return record;
    } catch (...) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while framing a UDP payload");
    }
}

asio::awaitable<common::Result<DatagramRelayStats>>
relay_tls_and_udp(TlsStream& tls_stream, asio::ip::udp::socket& udp_socket,
                  const DatagramRelayOptions options) {
    if (options.inactivity_timeout <= std::chrono::seconds::zero() ||
        options.inactivity_timeout > kMaximumRelayTimeout || !udp_socket.is_open()) {
        co_return common::Result<DatagramRelayStats>::failure(
            common::ErrorCode::invalid_argument, "UDP relay options or socket are invalid");
    }
    auto operation = std::make_shared<DatagramRelayOperation<TlsStream>>(tls_stream, udp_socket,
                                                                         options.inactivity_timeout);
    co_return co_await operation->run();
}

asio::awaitable<common::Result<DatagramRelayStats>>
relay_tcp_and_udp(asio::ip::tcp::socket& tcp_socket, asio::ip::udp::socket& udp_socket,
                  const DatagramRelayOptions options) {
    if (options.inactivity_timeout <= std::chrono::seconds::zero() ||
        options.inactivity_timeout > kMaximumRelayTimeout || !udp_socket.is_open() ||
        !tcp_socket.is_open()) {
        co_return common::Result<DatagramRelayStats>::failure(
            common::ErrorCode::invalid_argument, "UDP relay options or socket are invalid");
    }
    auto operation = std::make_shared<DatagramRelayOperation<asio::ip::tcp::socket>>(
        tcp_socket, udp_socket, options.inactivity_timeout);
    co_return co_await operation->run();
}

} // namespace minitun::protocol
