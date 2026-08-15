#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include <minitun/common/result.hpp>
#include <minitun/protocol/tls.hpp>

namespace minitun::protocol {

inline constexpr std::size_t kMaximumUdpPayloadSize = 65'507U;
inline constexpr std::size_t kDatagramRecordHeaderSize = 2U;

struct DatagramRelayOptions final {
    std::chrono::seconds inactivity_timeout{300};
};

struct DatagramRelayStats final {
    std::uint64_t tls_to_udp_bytes{0U};
    std::uint64_t udp_to_tls_bytes{0U};
    std::uint64_t tls_to_udp_datagrams{0U};
    std::uint64_t udp_to_tls_datagrams{0U};
    std::chrono::milliseconds duration{0};
};

/// Encodes one UDP payload as a two-byte network-order length followed by the
/// exact datagram bytes. Records are transported inside the authenticated TLS
/// Worker stream and preserve datagram boundaries.
[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_datagram_record(std::span<const std::uint8_t> payload);

[[nodiscard]] asio::awaitable<common::Result<DatagramRelayStats>>
relay_tls_and_udp(TlsStream& tls_stream, asio::ip::udp::socket& udp_socket,
                  DatagramRelayOptions options = {});

/// Relays length-prefixed UDP datagram records over a raw TCP socket,
/// mirroring relay_tls_and_udp for the P2P relay fallback path.
[[nodiscard]] asio::awaitable<common::Result<DatagramRelayStats>>
relay_tcp_and_udp(asio::ip::tcp::socket& tcp_socket, asio::ip::udp::socket& udp_socket,
                  DatagramRelayOptions options = {});

} // namespace minitun::protocol
