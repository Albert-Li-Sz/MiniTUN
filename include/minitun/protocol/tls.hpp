#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include <minitun/common/result.hpp>
#include <minitun/protocol/frame.hpp>

namespace minitun::protocol {

using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;

/// Thread-safe client-side cache shared by one remote server's control
/// connection and relay workers. Protocol PSK authentication still runs for
/// every resumed TLS transport.
class TlsSessionCache final {
  public:
    TlsSessionCache();
    ~TlsSessionCache() noexcept;

    TlsSessionCache(const TlsSessionCache&) = delete;
    TlsSessionCache& operator=(const TlsSessionCache&) = delete;
    TlsSessionCache(TlsSessionCache&&) = delete;
    TlsSessionCache& operator=(TlsSessionCache&&) = delete;

    [[nodiscard]] bool restore(TlsStream& stream) noexcept;
    [[nodiscard]] bool capture(TlsStream& stream) noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

struct ServerTlsContextOptions final {
    std::string certificate_chain_path;
    std::string private_key_path;
    /// When present, request and verify optional client certificates against this CA.
    std::string client_ca_certificate_path;
};

struct ClientTlsContextOptions final {
    std::string_view ca_certificate_path{};
    std::string_view ca_certificate_pem{};
    std::string_view client_certificate_pem{};
    std::string_view client_private_key_pem{};
};

[[nodiscard]] common::Result<std::shared_ptr<asio::ssl::context>>
make_server_tls_context(const ServerTlsContextOptions& options);

[[nodiscard]] common::Result<std::shared_ptr<asio::ssl::context>>
make_client_tls_context(const ClientTlsContextOptions& options);

[[nodiscard]] common::Result<void> configure_client_tls_stream(TlsStream& stream,
                                                               const std::string& server_name,
                                                               bool insecure_skip_verify);

/// Advisory settings for first-byte latency and dead-peer detection. A kernel
/// that rejects either setting does not make an otherwise valid connection fail.
template <typename Socket>
void configure_tcp_transport(Socket& socket) noexcept {
    asio::error_code ignored;
    socket.set_option(asio::ip::tcp::no_delay{true}, ignored);
    ignored.clear();
    socket.set_option(asio::socket_base::keep_alive{true}, ignored);
}

[[nodiscard]] bool tls_session_reused(TlsStream& stream) noexcept;

[[nodiscard]] asio::awaitable<common::Result<Frame>>
async_read_frame(TlsStream& stream, std::size_t max_frame_size = kMaxFrameSize);

[[nodiscard]] asio::awaitable<common::Result<void>>
async_write_frame(TlsStream& stream, Frame frame, std::size_t max_frame_size = kMaxFrameSize);

/// Writes one bounded pipeline window as a single TLS application write. This
/// avoids an inter-frame TLS read/write dependency while the peer is already
/// returning responses for earlier requests in the same window.
[[nodiscard]] asio::awaitable<common::Result<void>>
async_write_frames(TlsStream& stream, std::vector<Frame> frames,
                   std::size_t max_frame_size = kMaxFrameSize);

void close_tls_stream(TlsStream& stream) noexcept;

} // namespace minitun::protocol
