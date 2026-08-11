#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/version.hpp>
#include <minitun/gui/server.hpp>
#include <minitun/ipc/local_client.hpp>

#ifndef MINITUN_GUI_ASSETS_DIR
#define MINITUN_GUI_ASSETS_DIR "/usr/share/minitun/gui"
#endif

int main(int argc, char** argv) {
    CLI::App app{"MiniTun localhost web management console"};
    std::string listen{"127.0.0.1:6500"};
    std::string socket_path{minitun::ipc::kDefaultSocketPath};
    std::string assets_directory{MINITUN_GUI_ASSETS_DIR};
    bool show_version = false;
    app.add_option("--listen", listen, "Numeric loopback listen endpoint")->capture_default_str();
    app.add_option("--socket", socket_path, "minitund local IPC socket")->capture_default_str();
    app.add_option("--assets-dir", assets_directory, "Built GUI asset directory")
        ->capture_default_str();
    app.add_flag("--version", show_version, "Print version and exit");
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }
    if (show_version) {
        std::cout << minitun::common::format_version_info("minitun-gui") << '\n';
        return EXIT_SUCCESS;
    }

    auto listen_endpoint = minitun::common::Endpoint::parse(listen);
    if (!listen_endpoint) {
        std::cerr << "minitun-gui: " << listen_endpoint.error().message() << '\n';
        return EXIT_FAILURE;
    }

    asio::io_context io_context;
    auto server =
        minitun::gui::Server::create(io_context, {.listen_endpoint = listen,
                                                  .socket_path = std::move(socket_path),
                                                  .assets_directory = std::move(assets_directory)});
    if (!server) {
        std::cerr << "minitun-gui: " << server.error().message() << '\n';
        return EXIT_FAILURE;
    }
    auto started = (*server)->start();
    if (!started) {
        std::cerr << "minitun-gui: " << started.error().message() << '\n';
        return EXIT_FAILURE;
    }
    const auto display_host =
        listen_endpoint->is_ipv6() ? '[' + listen_endpoint->host() + ']' : listen_endpoint->host();
    std::cout << "MiniTun GUI listening on http://" << display_host << ':'
              << (*server)->listening_port() << '\n';

    asio::signal_set signals{io_context, SIGINT, SIGTERM};
    signals.async_wait([&server, &io_context](const asio::error_code&, const int) {
        (*server)->stop();
        io_context.stop();
    });
    io_context.run();
    return EXIT_SUCCESS;
}
