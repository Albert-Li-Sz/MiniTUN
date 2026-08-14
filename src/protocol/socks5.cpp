#include <minitun/protocol/socks5.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/error.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <minitun/common/error.hpp>

namespace minitun::protocol {
namespace {

inline constexpr std::uint8_t kVersion = 0x05U;
inline constexpr std::uint8_t kNoAuthentication = 0x00U;
inline constexpr std::uint8_t kNoAcceptableMethod = 0xffU;
inline constexpr std::uint8_t kConnect = 0x01U;
inline constexpr std::uint8_t kIpv4 = 0x01U;
inline constexpr std::uint8_t kDomain = 0x03U;
inline constexpr std::uint8_t kIpv6 = 0x04U;

[[nodiscard]] asio::awaitable<bool> read_exact(TlsStream& stream, void* data,
                                               const std::size_t size) {
    asio::error_code error;
    const std::size_t read = co_await asio::async_read(
        stream, asio::buffer(data, size), asio::redirect_error(asio::use_awaitable, error));
    co_return !error && read == size;
}

[[nodiscard]] asio::awaitable<bool> write_exact(TlsStream& stream, const void* data,
                                                const std::size_t size) {
    asio::error_code error;
    const std::size_t written = co_await asio::async_write(
        stream, asio::buffer(data, size), asio::redirect_error(asio::use_awaitable, error));
    co_return !error && written == size;
}

[[nodiscard]] asio::awaitable<void> send_failure(TlsStream& stream, const std::uint8_t reply) {
    const std::array<std::uint8_t, 10U> response{kVersion, reply, 0U, kIpv4, 0U,
                                                 0U,       0U,    0U, 0U,    0U};
    static_cast<void>(co_await write_exact(stream, response.data(), response.size()));
}

[[nodiscard]] std::vector<std::uint8_t>
success_response(const asio::ip::tcp::socket& destination_socket) {
    asio::error_code error;
    const auto endpoint = destination_socket.local_endpoint(error);
    if (error || endpoint.address().is_v4()) {
        const auto bytes =
            error ? asio::ip::address_v4{}.to_bytes() : endpoint.address().to_v4().to_bytes();
        const std::uint16_t port = error ? 0U : endpoint.port();
        return {kVersion,
                0U,
                0U,
                kIpv4,
                bytes[0],
                bytes[1],
                bytes[2],
                bytes[3],
                static_cast<std::uint8_t>((port >> 8U) & 0xffU),
                static_cast<std::uint8_t>(port & 0xffU)};
    }

    const auto bytes = endpoint.address().to_v6().to_bytes();
    const std::uint16_t port = endpoint.port();
    std::vector<std::uint8_t> response{kVersion, 0U, 0U, kIpv6};
    response.insert(response.end(), bytes.begin(), bytes.end());
    response.push_back(static_cast<std::uint8_t>((port >> 8U) & 0xffU));
    response.push_back(static_cast<std::uint8_t>(port & 0xffU));
    return response;
}

} // namespace

asio::awaitable<common::Result<void>>
accept_socks5_connect(TlsStream& client_stream, asio::ip::tcp::socket& destination_socket,
                      asio::ip::tcp::resolver& resolver) {
    std::array<std::uint8_t, 2U> greeting{};
    if (!co_await read_exact(client_stream, greeting.data(), greeting.size()) ||
        greeting[0] != kVersion || greeting[1] == 0U) {
        co_return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                                "SOCKS5 greeting is invalid");
    }

    std::vector<std::uint8_t> methods(greeting[1]);
    if (!co_await read_exact(client_stream, methods.data(), methods.size())) {
        co_return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                                "SOCKS5 methods are truncated");
    }
    const bool no_auth =
        std::find(methods.begin(), methods.end(), kNoAuthentication) != methods.end();
    const std::array<std::uint8_t, 2U> selection{kVersion,
                                                 no_auth ? kNoAuthentication : kNoAcceptableMethod};
    if (!co_await write_exact(client_stream, selection.data(), selection.size()) || !no_auth) {
        co_return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                                "SOCKS5 client offered no supported method");
    }

    std::array<std::uint8_t, 4U> request{};
    if (!co_await read_exact(client_stream, request.data(), request.size()) ||
        request[0] != kVersion || request[2] != 0U) {
        co_return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                                "SOCKS5 request is invalid");
    }
    if (request[1] != kConnect) {
        co_await send_failure(client_stream, 0x07U);
        co_return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                "SOCKS5 command is not supported");
    }

    std::string host;
    if (request[3] == kIpv4) {
        std::array<std::uint8_t, 4U> bytes{};
        if (!co_await read_exact(client_stream, bytes.data(), bytes.size())) {
            co_return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                                    "SOCKS5 IPv4 target is truncated");
        }
        host = asio::ip::address_v4{bytes}.to_string();
    } else if (request[3] == kIpv6) {
        std::array<std::uint8_t, 16U> bytes{};
        if (!co_await read_exact(client_stream, bytes.data(), bytes.size())) {
            co_return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                                    "SOCKS5 IPv6 target is truncated");
        }
        host = asio::ip::address_v6{bytes}.to_string();
    } else if (request[3] == kDomain) {
        std::uint8_t length = 0U;
        if (!co_await read_exact(client_stream, &length, 1U) || length == 0U) {
            co_return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                                    "SOCKS5 domain target is invalid");
        }
        std::vector<std::uint8_t> bytes(length);
        if (!co_await read_exact(client_stream, bytes.data(), bytes.size()) ||
            std::find(bytes.begin(), bytes.end(), 0U) != bytes.end()) {
            co_return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                                    "SOCKS5 domain target is invalid");
        }
        host.assign(bytes.begin(), bytes.end());
    } else {
        co_await send_failure(client_stream, 0x08U);
        co_return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                "SOCKS5 address type is not supported");
    }

    std::array<std::uint8_t, 2U> port_bytes{};
    if (!co_await read_exact(client_stream, port_bytes.data(), port_bytes.size())) {
        co_return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                                "SOCKS5 target port is truncated");
    }
    const std::uint16_t port = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(port_bytes[0]) << 8U) | port_bytes[1]);
    if (port == 0U) {
        co_await send_failure(client_stream, 0x01U);
        co_return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                "SOCKS5 target port is zero");
    }

    asio::error_code error;
    auto endpoints = co_await resolver.async_resolve(
        host, std::to_string(port), asio::redirect_error(asio::use_awaitable, error));
    if (!error) {
        co_await asio::async_connect(destination_socket, endpoints,
                                     asio::redirect_error(asio::use_awaitable, error));
    }
    if (error) {
        const std::uint8_t reply = error == asio::error::connection_refused ? 0x05U : 0x04U;
        co_await send_failure(client_stream, reply);
        co_return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                "SOCKS5 destination connection failed");
    }

    const auto response = success_response(destination_socket);
    if (!co_await write_exact(client_stream, response.data(), response.size())) {
        co_return common::Result<void>::failure(common::ErrorCode::connection_failed,
                                                "SOCKS5 success reply could not be sent");
    }
    co_return common::Result<void>::success();
}

} // namespace minitun::protocol
