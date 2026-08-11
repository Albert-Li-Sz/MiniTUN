#pragma once

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>

#include <minitun/common/result.hpp>
#include <minitun/protocol/tls.hpp>

namespace minitun::protocol {

/// Completes a SOCKS5 no-authentication CONNECT handshake carried by an
/// assigned MiniTun Worker and connects `destination_socket` to the requested
/// target. The caller owns timeout and access-policy enforcement.
[[nodiscard]] asio::awaitable<common::Result<void>>
accept_socks5_connect(TlsStream& client_stream, asio::ip::tcp::socket& destination_socket,
                      asio::ip::tcp::resolver& resolver);

} // namespace minitun::protocol
