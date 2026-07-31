#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>

#include <minitun/common/result.hpp>
#include <minitun/protocol/tls.hpp>

namespace minitun::protocol {

inline constexpr std::size_t kRelayBufferSize = 16U * 1024U;

struct RelayOptions final {
    std::chrono::seconds inactivity_timeout{300};
};

struct RelayStats final {
    std::uint64_t tls_to_tcp_bytes{0U};
    std::uint64_t tcp_to_tls_bytes{0U};
    std::chrono::milliseconds duration{0};

    friend bool operator==(const RelayStats&, const RelayStats&) = default;
};

[[nodiscard]] asio::awaitable<common::Result<RelayStats>>
relay_tls_and_tcp(TlsStream& tls_stream, asio::ip::tcp::socket& tcp_socket,
                  RelayOptions options = {});

} // namespace minitun::protocol
