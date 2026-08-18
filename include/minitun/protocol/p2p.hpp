#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

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

/// Payload carried by an established P2P path: a raw TCP relay or framed
/// UDP datagram records.
enum class P2pTransport : std::uint8_t {
    tcp,
    udp,
};

struct P2pHostUpgrade final {
    P2pPath path{P2pPath::relay};
    P2pTransport transport{P2pTransport::tcp};
    /// Direct path only: the accepted candidate socket upgraded to TLS 1.3
    /// with the one-time rendezvous token as an external PSK.
    std::unique_ptr<TlsStream> direct_stream;
};

struct P2pPeerUpgrade final {
    P2pPath path{P2pPath::relay};
    P2pTransport transport{P2pTransport::tcp};
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
/// already-established TLS Worker stream. When simultaneous open is enabled
/// and the server observed the peer's public endpoint, a peer-side MTPS
/// request makes the host connect back to that endpoint from the same local
/// port as the direct listener so two endpoint-independent NAT mappings can
/// punch through. A successful direct candidate is upgraded to TLS 1.3 with
/// the one-time token as an external PSK before it is returned.
///
/// `candidate_address` is the address the direct listener binds to (the
/// Worker's local address). `advertised_address`, when set, is the Worker's
/// public address as observed by the server and is what the offer hands the
/// peer instead of the local address, so a NAT'd Worker still advertises a
/// reachable candidate. It must share `candidate_address`'s address family.
[[nodiscard]] asio::awaitable<common::Result<P2pHostUpgrade>>
accept_p2p_upgrade(TlsStream& relay_stream, const asio::ip::address& candidate_address,
                   std::optional<asio::ip::address> advertised_address = std::nullopt,
                   std::chrono::seconds negotiation_timeout = std::chrono::seconds{5},
                   std::optional<asio::ip::tcp::endpoint> peer_observed_endpoint = std::nullopt,
                   bool simultaneous_open_enabled = false);

/// Consumes a MiniTun P2P offer from `bootstrap_socket`, tries the advertised
/// direct candidate, and otherwise keeps the bootstrap socket as the relay.
/// With simultaneous open enabled, a failed direct attempt requests MTPS and
/// then reconnects to the candidate from the same local port so both sides
/// punch their NAT mappings; relay fallback follows when that also fails.
[[nodiscard]] asio::awaitable<common::Result<P2pPeerUpgrade>>
connect_p2p_upgrade(asio::ip::tcp::socket bootstrap_socket,
                    std::chrono::seconds negotiation_timeout = std::chrono::seconds{5},
                    std::chrono::seconds direct_connect_timeout = std::chrono::seconds{2},
                    bool direct_enabled = true, bool simultaneous_open_enabled = true,
                    P2pTransport transport = P2pTransport::tcp);

/// Creates the outbound half of a TCP simultaneous open: a connecting socket
/// bound to the same local port as the direct listener so both NAT mappings
/// share the punch port. Platforms that refuse the shared port degrade to an
/// ephemeral source port; mismatched address families are rejected.
[[nodiscard]] common::Result<std::shared_ptr<asio::ip::tcp::socket>>
create_simultaneous_open_socket(const asio::any_io_executor& executor,
                                const asio::ip::tcp::endpoint& listener_endpoint,
                                const asio::ip::tcp::endpoint& peer_endpoint);

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
