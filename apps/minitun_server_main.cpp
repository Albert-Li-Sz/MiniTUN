#include <CLI/CLI.hpp>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <minitun/admin/server.hpp>
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

[[nodiscard]] std::string server_metrics_text(const minitun::server::Server& server) {
    const auto metrics = server.metrics();
    const auto version = minitun::common::version_info();
    std::ostringstream output;
    output << "# TYPE minitun_build_info gauge\n"
           << "minitun_build_info{role=\"server\",version=\"" << version.version
           << "\",protocol=\"" << version.protocol_version << "\"} 1\n"
           << "# TYPE minitun_sessions gauge\nminitun_sessions " << metrics.active_sessions
           << "\n# TYPE minitun_connections gauge\n"
           << "minitun_connections{state=\"active\"} " << metrics.active_connections
           << "\nminitun_connections{state=\"pending\"} " << metrics.pending_connections
           << "\n# TYPE minitun_tunnels gauge\nminitun_tunnels " << metrics.active_tunnels
           << "\n# TYPE minitun_workers gauge\nminitun_workers{state=\"idle\"} "
           << metrics.idle_workers << "\n"
           << "# TYPE minitun_relays gauge\nminitun_relays{state=\"active\"} "
           << metrics.active_relays << "\n"
           << "# TYPE minitun_connections_total counter\nminitun_connections_total "
           << metrics.connections_total << "\n"
           << "# TYPE minitun_tls_resumptions_total counter\nminitun_tls_resumptions_total "
           << metrics.tls_resumptions_total << "\n"
           << "# TYPE minitun_authentication_total counter\n"
           << "minitun_authentication_total{result=\"success\"} "
           << metrics.authentication_success_total << "\n"
           << "minitun_authentication_total{result=\"failure\"} "
           << metrics.authentication_failure_total << "\n"
           << "# TYPE minitun_registrations_total counter\n"
           << "minitun_registrations_total{result=\"success\"} "
           << metrics.registration_success_total << "\n"
           << "minitun_registrations_total{result=\"failure\"} "
           << metrics.registration_failure_total << "\n"
           << "# TYPE minitun_unregistrations_total counter\nminitun_unregistrations_total "
           << metrics.unregistration_total << "\n"
           << "# TYPE minitun_relays_total counter\nminitun_relays_total "
           << metrics.relay_total << "\n"
           << "# TYPE minitun_relay_bytes_total counter\n"
           << "minitun_relay_bytes_total{direction=\"in\"} " << metrics.relay_bytes_in_total
           << "\nminitun_relay_bytes_total{direction=\"out\"} "
           << metrics.relay_bytes_out_total << "\n"
           << "# TYPE minitun_acl_rejections_total counter\nminitun_acl_rejections_total "
           << metrics.acl_rejections_total << "\n"
           << "# TYPE minitun_quota_rejections_total counter\nminitun_quota_rejections_total "
           << metrics.quota_rejections_total << "\n"
           << "# TYPE minitun_errors_total counter\nminitun_errors_total "
           << metrics.errors_total << "\n"
           << "# TYPE minitun_policy_reloads_total counter\n"
           << "minitun_policy_reloads_total{result=\"success\"} "
           << metrics.policy_reloads_total << "\n"
           << "minitun_policy_reloads_total{result=\"failure\"} "
           << metrics.policy_reload_failures_total << "\n"
           << "# TYPE minitun_registration_latency_seconds summary\n"
           << "minitun_registration_latency_seconds_count "
           << metrics.registration_success_total + metrics.registration_failure_total << "\n"
           << "minitun_registration_latency_seconds_sum "
           << static_cast<long double>(metrics.registration_latency_microseconds_total) /
                  1'000'000.0L
           << "\n";
    return output.str();
}

int run_server(const minitun::server::ServerOptions& options,
               const minitun::admin::ServerOptions& admin_options,
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
    const auto ready = std::make_shared<std::atomic_bool>(false);
    std::unique_ptr<minitun::admin::Server> admin_server;
    if (!admin_options.listen_endpoint.empty()) {
        auto configured = minitun::admin::Server::create(
            io_context, admin_options,
            {
                .healthy = [] { return true; },
                .ready = [ready] { return ready->load(std::memory_order_relaxed); },
                .metrics = [server = server->get()] { return server_metrics_text(*server); },
            });
        if (!configured) {
            std::cerr << "minitun-server: invalid admin listener: " << configured.error() << '\n';
            return configured.error().code() == minitun::common::ErrorCode::invalid_argument ||
                           configured.error().code() == minitun::common::ErrorCode::permission_denied
                       ? kInvalidArgumentsExitCode
                       : kInternalErrorExitCode;
        }
        admin_server = std::move(*configured);
    }
    auto started = (*server)->start();
    if (!started) {
        std::cerr << "minitun-server: failed to start listener: " << started.error() << '\n';
        return kInternalErrorExitCode;
    }
    ready->store(true, std::memory_order_relaxed);
    if (admin_server != nullptr) {
        started = admin_server->start();
        if (!started) {
            ready->store(false, std::memory_order_relaxed);
            (*server)->stop();
            io_context.poll();
            std::cerr << "minitun-server: failed to start admin listener: " << started.error()
                      << '\n';
            return kInternalErrorExitCode;
        }
    }

    minitun::common::log_info("TLS server started", {.component = "server",
                                                     .server_id = (*server)->server_id(),
                                                     .remote_endpoint = options.listen_endpoint});

    asio::signal_set signals{io_context, SIGINT, SIGTERM, SIGHUP};
    auto signal_handler = std::make_shared<std::function<void(const asio::error_code&, int)>>();
    const std::weak_ptr weak_signal_handler = signal_handler;
    *signal_handler = [&server, &admin_server, &signals, &ready, weak_signal_handler](
                          const asio::error_code& error, const int signal_number) {
        if (error) {
            return;
        }
        if (signal_number == SIGHUP) {
            auto reloaded = (*server)->reload();
            if (!reloaded) {
                minitun::common::log_error(
                    "TLS credential or client policy reload failed",
                    {.component = "server", .error_code = reloaded.error().code()});
            }
            if (const auto handler = weak_signal_handler.lock()) {
                signals.async_wait(*handler);
            }
            return;
        }
        ready->store(false, std::memory_order_relaxed);
        if (admin_server != nullptr) {
            admin_server->stop();
        }
        (*server)->stop();
    };
    signals.async_wait(*signal_handler);

    std::vector<std::thread> workers;
    workers.reserve(io_threads > 0U ? io_threads - 1U : 0U);
    std::mutex failure_mutex;
    std::exception_ptr worker_failure;
    const auto record_failure = [&io_context, &failure_mutex,
                                 &worker_failure](const std::exception_ptr& failure) {
        {
            const std::scoped_lock lock{failure_mutex};
            if (!worker_failure) {
                worker_failure = failure;
            }
        }
        io_context.stop();
    };
    for (std::size_t index = 1U; index < io_threads; ++index) {
        workers.emplace_back([&io_context, &record_failure] {
            try {
                io_context.run();
            } catch (...) {
                record_failure(std::current_exception());
            }
        });
    }

    try {
        io_context.run();
    } catch (...) {
        record_failure(std::current_exception());
    }
    for (auto& worker : workers) {
        worker.join();
    }
    ready->store(false, std::memory_order_relaxed);
    if (admin_server != nullptr) {
        admin_server->stop();
    }
    (*server)->stop();
    {
        const std::scoped_lock lock{failure_mutex};
        if (worker_failure) {
            try {
                std::rethrow_exception(worker_failure);
            } catch (const std::exception& exception) {
                minitun::common::log_error(
                    std::string{"server I/O worker failed: "} + exception.what(),
                    {.component = "server",
                     .error_code = minitun::common::ErrorCode::internal_error});
            } catch (...) {
                minitun::common::log_error(
                    "server I/O worker failed with a non-standard exception",
                    {.component = "server",
                     .error_code = minitun::common::ErrorCode::internal_error});
            }
            return kInternalErrorExitCode;
        }
    }
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
    minitun::admin::ServerOptions admin_options;
    app.add_option("--listen", options.listen_endpoint, "TLS control listener endpoint")
        ->capture_default_str();
    app.add_option("--tls-cert", options.tls_certificate_path, "PEM certificate chain")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--tls-key", options.tls_private_key_path, "PEM private key")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--clients-config", options.clients_config_path,
                   "Strict JSON client policy configuration")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--client-ca", options.client_ca_path,
                   "PEM CA used to verify policy-bound client certificates")
        ->check(CLI::ExistingFile);
    app.add_option("--admin-listen", admin_options.listen_endpoint,
                   "Optional numeric management listener for health and metrics");
    app.add_option("--admin-token-file", admin_options.token_file,
                   "Private Bearer token file required for non-loopback management listeners")
        ->check(CLI::ExistingFile);
    app.add_option("--max-clients", options.max_clients, "Maximum authenticated clients")
        ->check(CLI::Range(1U, 100'000U))
        ->capture_default_str();
    app.add_option("--max-tunnels-per-client", options.max_tunnels_per_client,
                   "Maximum registered tunnels per authenticated client")
        ->check(CLI::Range(1U, 4'096U))
        ->capture_default_str();
    app.add_option("--max-total-tunnels", options.max_total_tunnels,
                   "Maximum registered tunnels across all authenticated clients")
        ->check(CLI::Range(1U, 100'000U))
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

    return run_server(options, admin_options, *log_level, io_threads);
}
