#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/redirect_error.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/version.hpp>
#include <minitun/protocol/datagram.hpp>
#include <minitun/protocol/p2p.hpp>
#include <minitun/protocol/relay.hpp>
#include <minitun/protocol/tls.hpp>

namespace {

using asio::ip::tcp;
using asio::ip::udp;

void close_socket(tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

void close_udp_socket(udp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.close(ignored);
}

/// Writes one length-prefixed UDP datagram record to a TLS or raw TCP stream.
template <typename Stream>
[[nodiscard]] asio::awaitable<bool>
write_datagram_record(Stream& stream, const std::vector<std::uint8_t>& payload) {
    auto record = minitun::protocol::encode_datagram_record(
        std::span<const std::uint8_t>{payload.data(), payload.size()});
    if (!record) {
        co_return false;
    }
    asio::error_code error;
    const std::size_t written = co_await asio::async_write(
        stream, asio::buffer(*record), asio::redirect_error(asio::use_awaitable, error));
    co_return !error && written == record->size();
}

[[nodiscard]] std::string peer_key(const udp::endpoint& endpoint) {
    return endpoint.address().to_string() + ':' + std::to_string(endpoint.port());
}

/// One local UDP peer multiplexed over its own P2P path. The session owns a UDP
/// socket bound to the connector's listen port and connected to the peer, so the
/// datagram relay sends replies from the address the peer originally sent to.
class UdpSession final : public std::enable_shared_from_this<UdpSession> {
  public:
    UdpSession(const asio::any_io_executor& executor, const udp::endpoint& local_endpoint,
               const udp::endpoint& peer_endpoint,
               const minitun::common::Endpoint& remote_endpoint,
               const std::chrono::seconds connect_timeout,
               const std::chrono::seconds negotiation_timeout,
               const std::chrono::seconds direct_timeout,
               const std::chrono::seconds inactivity_timeout, const bool direct_enabled,
               const bool simultaneous_open_enabled)
        : socket_(executor), remote_endpoint_(remote_endpoint), connect_timeout_(connect_timeout),
          negotiation_timeout_(negotiation_timeout), direct_timeout_(direct_timeout),
          inactivity_timeout_(inactivity_timeout), direct_enabled_(direct_enabled),
          simultaneous_open_enabled_(simultaneous_open_enabled) {
        asio::error_code error;
        socket_.open(peer_endpoint.protocol(), error);
        if (!error) {
            socket_.set_option(asio::socket_base::reuse_address{true}, error);
        }
        if (!error) {
            socket_.bind(local_endpoint, error);
        }
        if (!error) {
            socket_.connect(peer_endpoint, error);
        }
        socket_ok_ = !error;
    }

    /// Queues a datagram received by the shared listener. Datagrams arriving
    /// before the relay starts are drained in order once the path is ready;
    /// after that the kernel routes the peer's datagrams to the connected
    /// socket, so the listener never sees them again.
    void deliver(std::vector<std::uint8_t> datagram) {
        if (!relay_started_) {
            pending_.push_back(std::move(datagram));
        }
    }

    [[nodiscard]] asio::awaitable<void> run();

  private:
    template <typename Stream>
    [[nodiscard]] asio::awaitable<bool> flush_pending(Stream& stream) {
        while (!pending_.empty()) {
            auto datagram = std::move(pending_.front());
            pending_.pop_front();
            if (!co_await write_datagram_record(stream, datagram)) {
                co_return false;
            }
        }
        co_return true;
    }

    udp::socket socket_;
    minitun::common::Endpoint remote_endpoint_;
    std::chrono::seconds connect_timeout_;
    std::chrono::seconds negotiation_timeout_;
    std::chrono::seconds direct_timeout_;
    std::chrono::seconds inactivity_timeout_;
    bool direct_enabled_;
    bool simultaneous_open_enabled_;
    std::deque<std::vector<std::uint8_t>> pending_;
    bool relay_started_{false};
    bool socket_ok_{false};
};

[[nodiscard]] asio::awaitable<void> UdpSession::run() {
    auto self = shared_from_this();
    if (!socket_ok_) {
        co_return;
    }
    auto resolver = std::make_shared<tcp::resolver>(socket_.get_executor());
    auto bootstrap = std::make_shared<tcp::socket>(socket_.get_executor());
    asio::steady_timer timer{socket_.get_executor()};
    timer.expires_after(connect_timeout_);
    timer.async_wait([resolver, bootstrap](const asio::error_code& error) {
        if (!error) {
            resolver->cancel();
            close_socket(*bootstrap);
        }
    });
    asio::error_code error;
    auto endpoints = co_await resolver->async_resolve(
        remote_endpoint_.host(), std::to_string(remote_endpoint_.port()),
        asio::redirect_error(asio::use_awaitable, error));
    if (!error) {
        co_await asio::async_connect(*bootstrap, endpoints,
                                     asio::redirect_error(asio::use_awaitable, error));
    }
    static_cast<void>(timer.cancel());
    if (error) {
        co_return;
    }
    minitun::protocol::configure_tcp_transport(*bootstrap);
    auto upgraded = co_await minitun::protocol::connect_p2p_upgrade(
        std::move(*bootstrap), negotiation_timeout_, direct_timeout_, direct_enabled_,
        simultaneous_open_enabled_, minitun::protocol::P2pTransport::udp);
    if (!upgraded ||
        (upgraded->path == minitun::protocol::P2pPath::direct ? upgraded->direct_stream == nullptr
                                                              : upgraded->socket == nullptr)) {
        co_return;
    }
    std::clog << "minitun-p2p: selected "
              << (upgraded->path == minitun::protocol::P2pPath::direct ? "direct" : "relay")
              << " path (udp)\n";
    if (upgraded->path == minitun::protocol::P2pPath::direct) {
        if (!co_await flush_pending(*upgraded->direct_stream)) {
            co_return;
        }
        relay_started_ = true;
        static_cast<void>(co_await minitun::protocol::relay_tls_and_udp(
            *upgraded->direct_stream, socket_,
            {.inactivity_timeout = inactivity_timeout_}));
        co_return;
    }
    minitun::protocol::configure_tcp_transport(*upgraded->socket);
    if (!co_await flush_pending(*upgraded->socket)) {
        co_return;
    }
    relay_started_ = true;
    static_cast<void>(co_await minitun::protocol::relay_tcp_and_udp(
        *upgraded->socket, socket_, {.inactivity_timeout = inactivity_timeout_}));
    co_return;
}

[[nodiscard]] asio::awaitable<void>
accept_udp(udp::socket& socket, const minitun::common::Endpoint& remote_endpoint,
           const std::chrono::seconds connect_timeout,
           const std::chrono::seconds negotiation_timeout,
           const std::chrono::seconds direct_timeout,
           const std::chrono::seconds inactivity_timeout, const bool direct_enabled,
           const bool simultaneous_open_enabled) {
    asio::error_code error;
    const udp::endpoint local_endpoint = socket.local_endpoint(error);
    if (error) {
        co_return;
    }
    std::unordered_map<std::string, std::weak_ptr<UdpSession>> sessions;
    std::array<std::uint8_t, minitun::protocol::kMaximumUdpPayloadSize> buffer{};
    while (socket.is_open()) {
        udp::endpoint peer;
        const std::size_t bytes = co_await socket.async_receive_from(
            asio::buffer(buffer), peer, asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        std::vector<std::uint8_t> datagram(
            buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
        const std::string key = peer_key(peer);
        auto it = sessions.find(key);
        std::shared_ptr<UdpSession> session;
        if (it != sessions.end()) {
            session = it->second.lock();
        }
        if (!session) {
            session = std::make_shared<UdpSession>(
                socket.get_executor(), local_endpoint, peer, remote_endpoint, connect_timeout,
                negotiation_timeout, direct_timeout, inactivity_timeout, direct_enabled,
                simultaneous_open_enabled);
            sessions[key] = session;
            asio::co_spawn(socket.get_executor(), session->run(),
                           [session](const std::exception_ptr& failure) {
                               static_cast<void>(failure);
                           });
        }
        session->deliver(std::move(datagram));
    }
}

[[nodiscard]] asio::awaitable<void>
handle_connection(tcp::socket local_socket, minitun::common::Endpoint remote_endpoint,
                  const std::chrono::seconds connect_timeout,
                  const std::chrono::seconds negotiation_timeout,
                  const std::chrono::seconds direct_timeout,
                  const std::chrono::seconds inactivity_timeout, const bool direct_enabled,
                  const bool simultaneous_open_enabled) {
    auto resolver = std::make_shared<tcp::resolver>(local_socket.get_executor());
    auto bootstrap = std::make_shared<tcp::socket>(local_socket.get_executor());
    asio::steady_timer timer{local_socket.get_executor()};
    timer.expires_after(connect_timeout);
    timer.async_wait([resolver, bootstrap](const asio::error_code& error) {
        if (!error) {
            resolver->cancel();
            close_socket(*bootstrap);
        }
    });
    asio::error_code error;
    auto endpoints = co_await resolver->async_resolve(
        remote_endpoint.host(), std::to_string(remote_endpoint.port()),
        asio::redirect_error(asio::use_awaitable, error));
    if (!error) {
        co_await asio::async_connect(*bootstrap, endpoints,
                                     asio::redirect_error(asio::use_awaitable, error));
    }
    static_cast<void>(timer.cancel());
    if (error) {
        close_socket(local_socket);
        co_return;
    }
    minitun::protocol::configure_tcp_transport(*bootstrap);
    auto upgraded = co_await minitun::protocol::connect_p2p_upgrade(
        std::move(*bootstrap), negotiation_timeout, direct_timeout, direct_enabled,
        simultaneous_open_enabled);
    if (!upgraded || (upgraded->path == minitun::protocol::P2pPath::direct
                          ? upgraded->direct_stream == nullptr
                          : upgraded->socket == nullptr)) {
        close_socket(local_socket);
        co_return;
    }
    std::clog << "minitun-p2p: selected "
              << (upgraded->path == minitun::protocol::P2pPath::direct ? "direct" : "relay")
              << " path\n";
    if (upgraded->path == minitun::protocol::P2pPath::direct) {
        static_cast<void>(co_await minitun::protocol::relay_tls_and_tcp(
            *upgraded->direct_stream, local_socket,
            {.inactivity_timeout = inactivity_timeout}));
        co_return;
    }
    minitun::protocol::configure_tcp_transport(*upgraded->socket);
    static_cast<void>(co_await minitun::protocol::relay_tcp_and_tcp(local_socket, *upgraded->socket,
                                                                    inactivity_timeout));
}

[[nodiscard]] asio::awaitable<void>
accept_connections(tcp::acceptor& acceptor, const minitun::common::Endpoint& remote_endpoint,
                   const std::chrono::seconds connect_timeout,
                   const std::chrono::seconds negotiation_timeout,
                   const std::chrono::seconds direct_timeout,
                   const std::chrono::seconds inactivity_timeout, const bool direct_enabled,
                   const bool simultaneous_open_enabled) {
    while (acceptor.is_open()) {
        asio::error_code error;
        tcp::socket local =
            co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        asio::co_spawn(acceptor.get_executor(),
                       handle_connection(std::move(local), remote_endpoint, connect_timeout,
                                         negotiation_timeout, direct_timeout, inactivity_timeout,
                                         direct_enabled, simultaneous_open_enabled),
                       [](const std::exception_ptr&) {});
    }
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"MiniTun P2P connector with automatic relay fallback"};
    std::string remote_text;
    std::string listen_text{"127.0.0.1:6501"};
    std::uint16_t connect_timeout_seconds{10U};
    std::uint16_t negotiation_timeout_seconds{5U};
    std::uint16_t direct_timeout_seconds{2U};
    std::uint32_t inactivity_timeout_seconds{300U};
    bool allow_non_loopback = false;
    bool relay_only = false;
    bool simultaneous_open = true;
    bool udp_mode = false;
    bool show_version = false;
    app.add_option("remote", remote_text, "P2P tunnel endpoint on minitun-server");
    app.add_option("--listen", listen_text, "Local numeric listen endpoint")->capture_default_str();
    app.add_option("--connect-timeout", connect_timeout_seconds,
                   "Server connection timeout in seconds")
        ->check(CLI::Range(1U, 300U));
    app.add_option("--negotiation-timeout", negotiation_timeout_seconds,
                   "P2P negotiation timeout in seconds")
        ->check(CLI::Range(1U, 300U));
    app.add_option("--direct-timeout", direct_timeout_seconds,
                   "Direct candidate timeout in seconds")
        ->check(CLI::Range(1U, 300U));
    app.add_option("--inactivity-timeout", inactivity_timeout_seconds,
                   "Relay inactivity timeout in seconds")
        ->check(CLI::Range(1U, 86'400U));
    app.add_flag("--allow-non-loopback", allow_non_loopback,
                 "Allow exposing the local connector beyond loopback");
    app.add_flag("--relay-only", relay_only,
                 "Disable direct candidates and always use server relay fallback");
    app.add_flag("--udp", udp_mode,
                 "Forward local UDP datagrams over the P2P path instead of TCP");
    app.add_flag("--simultaneous-open,!--no-simultaneous-open", simultaneous_open,
                 "Try a TCP simultaneous open after a failed direct candidate")
        ->capture_default_str();
    app.add_flag("--version", show_version, "Print version and exit");
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }
    if (show_version) {
        std::cout << minitun::common::format_version_info("minitun-p2p") << '\n';
        return EXIT_SUCCESS;
    }
    if (remote_text.empty()) {
        std::cerr << "minitun-p2p: remote endpoint is required\n";
        return EXIT_FAILURE;
    }
    if (direct_timeout_seconds > negotiation_timeout_seconds) {
        std::cerr << "minitun-p2p: direct timeout cannot exceed negotiation timeout\n";
        return EXIT_FAILURE;
    }
    auto remote_endpoint = minitun::common::Endpoint::parse(remote_text);
    auto listen_endpoint = minitun::common::Endpoint::parse(listen_text);
    if (!remote_endpoint || !listen_endpoint) {
        std::cerr << "minitun-p2p: endpoint is invalid\n";
        return EXIT_FAILURE;
    }
    asio::error_code error;
    const auto listen_address = asio::ip::make_address(listen_endpoint->host(), error);
    if (error || (!allow_non_loopback && !listen_address.is_loopback())) {
        std::cerr << "minitun-p2p: local listener must use a numeric loopback address"
                     " unless --allow-non-loopback is set\n";
        return EXIT_FAILURE;
    }

    asio::io_context io_context;
    if (udp_mode) {
        udp::socket udp_socket{io_context};
        const udp::endpoint endpoint{listen_address, listen_endpoint->port()};
        udp_socket.open(endpoint.protocol(), error);
        if (!error) {
            udp_socket.set_option(asio::socket_base::reuse_address{true}, error);
        }
        if (!error) {
            udp_socket.bind(endpoint, error);
        }
        if (error) {
            std::cerr << "minitun-p2p: local listener could not be created\n";
            return EXIT_FAILURE;
        }
        const auto actual = udp_socket.local_endpoint(error);
        if (error) {
            return EXIT_FAILURE;
        }
        std::cout << "MiniTun P2P connector listening on " << actual.address().to_string() << ':'
                  << actual.port() << " (udp)\n";
        asio::co_spawn(io_context,
                       accept_udp(udp_socket, *remote_endpoint,
                                  std::chrono::seconds{connect_timeout_seconds},
                                  std::chrono::seconds{negotiation_timeout_seconds},
                                  std::chrono::seconds{direct_timeout_seconds},
                                  std::chrono::seconds{inactivity_timeout_seconds}, !relay_only,
                                  simultaneous_open),
                       [](const std::exception_ptr&) {});
        asio::signal_set signals{io_context, SIGINT, SIGTERM};
        signals.async_wait([&udp_socket, &io_context](const asio::error_code&, const int) {
            close_udp_socket(udp_socket);
            io_context.stop();
        });
        io_context.run();
        return EXIT_SUCCESS;
    }

    tcp::acceptor acceptor{io_context};
    const tcp::endpoint endpoint{listen_address, listen_endpoint->port()};
    acceptor.open(endpoint.protocol(), error);
    if (!error) {
        acceptor.set_option(tcp::acceptor::reuse_address{true}, error);
    }
    if (!error) {
        acceptor.bind(endpoint, error);
    }
    if (!error) {
        acceptor.listen(128, error);
    }
    if (error) {
        std::cerr << "minitun-p2p: local listener could not be created\n";
        return EXIT_FAILURE;
    }

    const auto actual = acceptor.local_endpoint(error);
    if (error) {
        return EXIT_FAILURE;
    }
    std::cout << "MiniTun P2P connector listening on " << actual.address().to_string() << ':'
              << actual.port() << '\n';
    asio::co_spawn(io_context,
                   accept_connections(
                       acceptor, *remote_endpoint, std::chrono::seconds{connect_timeout_seconds},
                       std::chrono::seconds{negotiation_timeout_seconds},
                       std::chrono::seconds{direct_timeout_seconds},
                       std::chrono::seconds{inactivity_timeout_seconds}, !relay_only,
                       simultaneous_open),
                   [](const std::exception_ptr&) {});
    asio::signal_set signals{io_context, SIGINT, SIGTERM};
    signals.async_wait([&acceptor, &io_context](const asio::error_code&, const int) {
        asio::error_code ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
        io_context.stop();
    });
    io_context.run();
    return EXIT_SUCCESS;
}
