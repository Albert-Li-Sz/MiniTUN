#include <CLI/CLI.hpp>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <minitun/common/error.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/version.hpp>
#include <minitun/server/server.hpp>

namespace {

constexpr int kSuccessExitCode = EXIT_SUCCESS;
constexpr int kInvalidArgumentsExitCode = 2;
constexpr int kInternalErrorExitCode = 10;

int parse_exit_code(CLI::App& app, const CLI::ParseError& error) {
    const int cli_exit_code = app.exit(error);
    return cli_exit_code == kSuccessExitCode ? kSuccessExitCode : kInvalidArgumentsExitCode;
}

class LoggingLifetime final {
  public:
    ~LoggingLifetime() { minitun::common::shutdown_logging(); }

    LoggingLifetime() = default;
    LoggingLifetime(const LoggingLifetime&) = delete;
    LoggingLifetime& operator=(const LoggingLifetime&) = delete;
};

[[nodiscard]] std::size_t default_io_threads() noexcept {
    const unsigned int detected = std::thread::hardware_concurrency();
    return std::min<std::size_t>(std::max<std::size_t>(detected, 1U), 16U);
}

int run_server(const minitun::server::ServerOptions& options,
               const minitun::common::LogLevel log_level, const std::size_t io_threads) {
    auto logging = minitun::common::initialize_logging({
        .logger_name = "minitun-server",
        .component = "server",
        .level = log_level,
    });
    if (!logging) {
        std::cerr << "minitun-server: failed to initialize logging: " << logging.error() << '\n';
        return kInternalErrorExitCode;
    }
    const LoggingLifetime logging_lifetime;

    asio::io_context io_context;
    auto server = minitun::server::Server::create(io_context, options);
    if (!server) {
        std::cerr << "minitun-server: startup validation failed: " << server.error() << '\n';
        return server.error().code() == minitun::common::ErrorCode::invalid_argument ||
                       server.error().code() == minitun::common::ErrorCode::permission_denied
                   ? kInvalidArgumentsExitCode
                   : kInternalErrorExitCode;
    }
    auto started = (*server)->start();
    if (!started) {
        std::cerr << "minitun-server: failed to start listener: " << started.error() << '\n';
        return kInternalErrorExitCode;
    }

    minitun::common::log_info("TLS server started", {.component = "server",
                                                     .server_id = (*server)->server_id(),
                                                     .remote_endpoint = options.listen_endpoint});

    asio::signal_set signals{io_context, SIGINT, SIGTERM};
    signals.async_wait([&server](const asio::error_code& error, int) {
        if (!error) {
            (*server)->stop();
        }
    });

    std::vector<std::jthread> workers;
    workers.reserve(io_threads > 0U ? io_threads - 1U : 0U);
    for (std::size_t index = 1U; index < io_threads; ++index) {
        workers.emplace_back([&io_context] {
            try {
                io_context.run();
            } catch (...) {
                io_context.stop();
            }
        });
    }

    try {
        io_context.run();
    } catch (...) {
        io_context.stop();
        return kInternalErrorExitCode;
    }
    for (auto& worker : workers) {
        worker.join();
    }
    (*server)->stop();
    minitun::common::log_info("TLS server stopped");
    return kSuccessExitCode;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{
        "MiniTun public server",
        "minitun-server",
    };

    const std::string version_info = minitun::common::format_version_info("minitun-server");
    app.set_version_flag("--version", version_info, "Show version information");

    minitun::server::ServerOptions options;
    app.add_option("--listen", options.listen_endpoint, "TLS control listener endpoint")
        ->capture_default_str();
    app.add_option("--tls-cert", options.tls_certificate_path, "PEM certificate chain")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--tls-key", options.tls_private_key_path, "PEM private key")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--token-file", options.token_file_path, "Private authentication Token file")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--allow-ports", options.allowed_ports,
                   "Allowed public tunnel port or inclusive range")
        ->capture_default_str();
    app.add_option("--max-clients", options.max_clients, "Maximum authenticated clients")
        ->check(CLI::Range(1U, 100'000U))
        ->capture_default_str();
    app.add_option("--max-tunnels-per-client", options.max_tunnels_per_client,
                   "Maximum registered tunnels per authenticated client")
        ->check(CLI::Range(1U, 4'096U))
        ->capture_default_str();
    app.add_option("--max-connections-per-client", options.max_connections_per_client,
                   "Maximum concurrent public relays per authenticated client")
        ->check(CLI::Range(1U, 100'000U))
        ->capture_default_str();
    app.add_option("--max-total-connections", options.max_total_connections,
                   "Maximum concurrent public relays across all clients")
        ->check(CLI::Range(1U, 100'000U))
        ->capture_default_str();
    app.add_option("--min-idle-workers", options.min_idle_workers,
                   "Minimum idle Workers requested per client session")
        ->check(CLI::Range(0U, 128U))
        ->capture_default_str();
    app.add_option("--max-idle-workers", options.max_idle_workers,
                   "Maximum idle Workers accepted per client session")
        ->check(CLI::Range(1U, 128U))
        ->capture_default_str();
    app.add_option("--max-total-idle-workers", options.max_total_idle_workers,
                   "Maximum idle Workers accepted across all clients")
        ->check(CLI::Range(1U, 4'096U))
        ->capture_default_str();

    int handshake_timeout_seconds = static_cast<int>(options.handshake_timeout.count());
    int heartbeat_interval_seconds = static_cast<int>(options.heartbeat_interval.count());
    int heartbeat_timeout_seconds = static_cast<int>(options.heartbeat_timeout.count());
    int clock_skew_seconds = static_cast<int>(options.allowed_clock_skew.count());
    int worker_wait_timeout_seconds = static_cast<int>(options.worker_wait_timeout.count());
    int worker_idle_timeout_seconds = static_cast<int>(options.worker_idle_timeout.count());
    int relay_idle_timeout_seconds = static_cast<int>(options.relay_inactivity_timeout.count());
    int shutdown_timeout_seconds = static_cast<int>(options.graceful_shutdown_timeout.count());
    app.add_option("--handshake-timeout", handshake_timeout_seconds,
                   "TLS and authentication timeout in seconds")
        ->check(CLI::Range(1, 300))
        ->capture_default_str();
    app.add_option("--heartbeat-interval", heartbeat_interval_seconds,
                   "Heartbeat interval in seconds")
        ->check(CLI::Range(1, 60))
        ->capture_default_str();
    app.add_option("--heartbeat-timeout", heartbeat_timeout_seconds,
                   "Heartbeat response timeout in seconds")
        ->check(CLI::Range(2, 300))
        ->capture_default_str();
    app.add_option("--auth-clock-skew", clock_skew_seconds,
                   "Maximum authentication clock skew in seconds")
        ->check(CLI::Range(0, 300))
        ->capture_default_str();
    app.add_option("--worker-wait-timeout", worker_wait_timeout_seconds,
                   "Maximum wait for an idle Worker in seconds")
        ->check(CLI::Range(1, 300))
        ->capture_default_str();
    app.add_option("--worker-idle-timeout", worker_idle_timeout_seconds,
                   "Idle Worker lifetime in seconds")
        ->check(CLI::Range(1, 300))
        ->capture_default_str();
    app.add_option("--relay-idle-timeout", relay_idle_timeout_seconds,
                   "Relay inactivity timeout in seconds")
        ->check(CLI::Range(1, 86'400))
        ->capture_default_str();
    app.add_option("--shutdown-timeout", shutdown_timeout_seconds,
                   "Maximum graceful relay drain time in seconds")
        ->check(CLI::Range(1, 300))
        ->capture_default_str();

    std::size_t io_threads = default_io_threads();
    app.add_option("--io-threads", io_threads, "Fixed Asio I/O thread count")
        ->check(CLI::Range(1U, 16U))
        ->capture_default_str();

    std::string log_level_text{"info"};
    app.add_option("--log-level", log_level_text,
                   "Log level: trace, debug, info, warn, error, critical, or off")
        ->capture_default_str();

    bool foreground = false;
    app.add_flag("--foreground", foreground, "Run in the foreground");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return parse_exit_code(app, error);
    }

    auto log_level = minitun::common::log_level_from_string(log_level_text);
    if (!log_level) {
        std::cerr << "minitun-server: " << log_level.error() << '\n';
        return kInvalidArgumentsExitCode;
    }
    options.handshake_timeout = std::chrono::seconds{handshake_timeout_seconds};
    options.heartbeat_interval = std::chrono::seconds{heartbeat_interval_seconds};
    options.heartbeat_timeout = std::chrono::seconds{heartbeat_timeout_seconds};
    options.allowed_clock_skew = std::chrono::seconds{clock_skew_seconds};
    options.worker_wait_timeout = std::chrono::seconds{worker_wait_timeout_seconds};
    options.worker_idle_timeout = std::chrono::seconds{worker_idle_timeout_seconds};
    options.relay_inactivity_timeout = std::chrono::seconds{relay_idle_timeout_seconds};
    options.graceful_shutdown_timeout = std::chrono::seconds{shutdown_timeout_seconds};
    static_cast<void>(foreground);

    return run_server(options, *log_level, io_threads);
}
