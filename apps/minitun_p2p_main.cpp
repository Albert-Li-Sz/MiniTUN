#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include <CLI/CLI.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/redirect_error.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/version.hpp>
#include <minitun/protocol/p2p.hpp>
#include <minitun/protocol/relay.hpp>
#include <minitun/protocol/tls.hpp>

namespace {

using asio::ip::tcp;

void close_socket(tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

[[nodiscard]] asio::awaitable<void>
handle_connection(tcp::socket local_socket, minitun::common::Endpoint remote_endpoint,
                  const std::chrono::seconds connect_timeout,
                  const std::chrono::seconds negotiation_timeout,
                  const std::chrono::seconds direct_timeout,
                  const std::chrono::seconds inactivity_timeout, const bool direct_enabled) {
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
        std::move(*bootstrap), negotiation_timeout, direct_timeout, direct_enabled);
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
                   const std::chrono::seconds inactivity_timeout, const bool direct_enabled) {
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
                                         direct_enabled),
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
                       std::chrono::seconds{inactivity_timeout_seconds}, !relay_only),
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
