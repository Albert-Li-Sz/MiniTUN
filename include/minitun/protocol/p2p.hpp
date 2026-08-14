#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <asio/awaitable.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include <minitun/common/result.hpp>
#include <minitun/protocol/tls.hpp>

namespace minitun::protocol {

enum class P2pPath : std::uint8_t {
    direct,
    relay,
};

struct P2pHostUpgrade final {
    P2pPath path{P2pPath::relay};
    /// Direct path only: the accepted candidate socket upgraded to TLS 1.3
    /// with the one-time rendezvous token as an external PSK.
    std::unique_ptr<TlsStream> direct_stream;
};

struct P2pPeerUpgrade final {
    P2pPath path{P2pPath::relay};
    /// Relay path only: the raw bootstrap socket kept as the TLS relay.
    std::unique_ptr<asio::ip::tcp::socket> socket;
    /// Direct path only: the candidate socket upgraded to TLS 1.3 with the
    /// one-time rendezvous token as an external PSK.
    std::unique_ptr<TlsStream> direct_stream;
};

struct P2pRelayStats final {
    std::uint64_t first_to_second_bytes{0U};
    std::uint64_t second_to_first_bytes{0U};
    std::chrono::milliseconds duration{0};

    friend bool operator==(const P2pRelayStats&, const P2pRelayStats&) = default;
};

/// Offers the address used by the authenticated Worker connection as a direct
/// TCP candidate. A peer that cannot reach it requests relay fallback over the
/// already-established TLS Worker stream. A successful direct candidate is
/// upgraded to TLS 1.3 with the one-time token as an external PSK before it is
/// returned.
[[nodiscard]] asio::awaitable<common::Result<P2pHostUpgrade>>
accept_p2p_upgrade(TlsStream& relay_stream, const asio::ip::address& candidate_address,
                   std::chrono::seconds negotiation_timeout = std::chrono::seconds{5});

/// Consumes a MiniTun P2P offer from `bootstrap_socket`, tries the advertised
/// direct candidate, and otherwise keeps the bootstrap socket as the relay.
[[nodiscard]] asio::awaitable<common::Result<P2pPeerUpgrade>>
connect_p2p_upgrade(asio::ip::tcp::socket bootstrap_socket,
                    std::chrono::seconds negotiation_timeout = std::chrono::seconds{5},
                    std::chrono::seconds direct_connect_timeout = std::chrono::seconds{2},
                    bool direct_enabled = true);

/// Confirms that the local service is ready on the selected direct path. The
/// stream is already TLS 1.3 with the rendezvous token as the PSK.
[[nodiscard]] asio::awaitable<common::Result<void>>
confirm_p2p_direct(TlsStream& stream);

/// Confirms that the local service is ready on the TLS relay fallback.
[[nodiscard]] asio::awaitable<common::Result<void>> confirm_p2p_relay(TlsStream& stream);

/// Relays a selected direct P2P connection to a local TCP service.
[[nodiscard]] asio::awaitable<common::Result<P2pRelayStats>>
relay_tcp_and_tcp(asio::ip::tcp::socket& first, asio::ip::tcp::socket& second,
                  std::chrono::seconds inactivity_timeout = std::chrono::seconds{300});

} // namespace minitun::protocol
