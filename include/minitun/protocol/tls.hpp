#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include <minitun/common/result.hpp>
#include <minitun/protocol/frame.hpp>

namespace minitun::protocol {

using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;

struct ServerTlsContextOptions final {
    std::string certificate_chain_path;
    std::string private_key_path;
};

struct ClientTlsContextOptions final {
    std::string ca_certificate_path;
};

[[nodiscard]] common::Result<std::shared_ptr<asio::ssl::context>>
make_server_tls_context(const ServerTlsContextOptions& options);

[[nodiscard]] common::Result<std::shared_ptr<asio::ssl::context>>
make_client_tls_context(const ClientTlsContextOptions& options);

[[nodiscard]] common::Result<void> configure_client_tls_stream(TlsStream& stream,
                                                               const std::string& server_name,
                                                               bool insecure_skip_verify);

[[nodiscard]] asio::awaitable<common::Result<Frame>>
async_read_frame(TlsStream& stream, std::size_t max_frame_size = kMaxFrameSize);

[[nodiscard]] asio::awaitable<common::Result<void>>
async_write_frame(TlsStream& stream, Frame frame, std::size_t max_frame_size = kMaxFrameSize);

void close_tls_stream(TlsStream& stream) noexcept;

} // namespace minitun::protocol
