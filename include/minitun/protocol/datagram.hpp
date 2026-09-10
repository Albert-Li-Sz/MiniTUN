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
/// The largest payload the 16-bit record length field can represent. This is
/// the framing ceiling, not a protocol limit: kMaximumUdpPayloadSize stays the
/// stricter UDP-over-IP policy bound and must remain below it.
inline constexpr std::size_t kMaximumDatagramRecordPayload = 0xFFFFU;
inline constexpr std::size_t kDatagramRecordHeaderSize = 2U;

static_assert(kMaximumUdpPayloadSize <= kMaximumDatagramRecordPayload,
              "the UDP policy limit must stay representable by the record length field");

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
///
/// Payloads above kMaximumUdpPayloadSize are rejected: the framing itself could
/// represent up to kMaximumDatagramRecordPayload bytes, but emitting a record
/// the daemon would refuse to relay serves no purpose. Rejection is what keeps
/// the 16-bit length field from ever wrapping.
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
