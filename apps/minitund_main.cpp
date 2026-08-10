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
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <minitun/admin/server.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/time.hpp>
#include <minitun/common/version.hpp>
#include <minitun/daemon/control_service.hpp>
#include <minitun/daemon/credential_keys.hpp>
#include <minitun/daemon/server_manager.hpp>
#include <minitun/ipc/dispatcher.hpp>
#include <minitun/ipc/local_server.hpp>
#include <minitun/ipc/protocol.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/database.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

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

[[nodiscard]] std::string daemon_metrics_text(minitun::daemon::ServerManager& manager) {
    const auto metrics = manager.metrics();
    const auto count = [&metrics](const std::string_view object,
                                  const std::string_view field) -> std::uint64_t {
        const auto parent = metrics.find(object);
        if (parent == metrics.end() || !parent->is_object()) {
            return 0U;
        }
        const auto value = parent->find(field);
        if (value == parent->end()) {
            return 0U;
        }
        if (value->is_number_unsigned()) {
            return value->get<std::uint64_t>();
        }
        return value->is_number_integer() && value->get<std::int64_t>() >= 0
                   ? static_cast<std::uint64_t>(value->get<std::int64_t>())
                   : 0U;
    };
    const auto scalar = [&metrics](const std::string_view field) -> std::uint64_t {
        const auto value = metrics.find(field);
        if (value == metrics.end()) {
            return 0U;
        }
        if (value->is_number_unsigned()) {
            return value->get<std::uint64_t>();
        }
        return value->is_number_integer() && value->get<std::int64_t>() >= 0
                   ? static_cast<std::uint64_t>(value->get<std::int64_t>())
                   : 0U;
    };
    const auto version = minitun::common::version_info();
    std::ostringstream output;
    output
        << "# TYPE minitun_build_info gauge\n"
        << "minitun_build_info{role=\"daemon\",version=\"" << version.version << "\",protocol=\""
        << version.protocol_version << "\"} 1\n"
        << "# TYPE minitun_sessions gauge\nminitun_sessions " << count("sessions", "active") << "\n"
        << "# TYPE minitun_workers gauge\nminitun_workers{state=\"idle\"} "
        << count("workers", "idle") << "\nminitun_workers{state=\"active\"} "
        << count("workers", "active") << "\n"
        << "# TYPE minitun_connections gauge\nminitun_connections{state=\"active\"} "
        << count("connections", "active") << "\nminitun_connections{state=\"pending\"} "
        << count("connections", "pending") << "\n"
        << "# TYPE minitun_reconnects_total counter\nminitun_reconnects_total "
        << scalar("reconnects") << "\n"
        << "# TYPE minitun_tls_resumptions_total counter\nminitun_tls_resumptions_total "
        << scalar("tls_resumptions") << "\n"
        << "# TYPE minitun_quota_rejections_total counter\nminitun_quota_rejections_total "
        << scalar("quota_rejections") << "\n"
        << "# TYPE minitun_errors_total counter\nminitun_errors_total " << scalar("errors") << "\n"
        << "# TYPE minitun_relay_bytes_total counter\nminitun_relay_bytes_total{direction=\"in\"} "
        << count("throughput", "bytes_in") << "\nminitun_relay_bytes_total{direction=\"out\"} "
        << count("throughput", "bytes_out") << "\n";
    return output.str();
}

[[nodiscard]] minitun::common::Result<void>
validate_recovered_credentials(minitun::storage::StateRepository& repository,
                               minitun::storage::CredentialStore& credentials,
                               const minitun::storage::RecoverySnapshot& snapshot) {
    for (auto server : snapshot.servers) {
        if (server.desired_state == minitun::storage::ServerDesiredState::removed) {
            auto removed = minitun::daemon::cleanup_all_server_credentials(credentials, server);
            if (!removed) {
                return removed;
            }
            auto erased = repository.servers().erase(server.id);
            if (!erased && erased.error().code() != minitun::common::ErrorCode::not_found) {
                return erased;
            }
            continue;
        }

        bool record_changed = false;
        const auto clear_reference = [&server](const minitun::daemon::ServerCredentialKind kind) {
            switch (kind) {
            case minitun::daemon::ServerCredentialKind::psk:
                server.credential_ref.reset();
                break;
            case minitun::daemon::ServerCredentialKind::ca_certificate:
                server.ca_credential_ref.reset();
                break;
            case minitun::daemon::ServerCredentialKind::client_certificate:
                server.client_certificate_ref.reset();
                break;
            case minitun::daemon::ServerCredentialKind::client_private_key:
                server.client_private_key_ref.reset();
                break;
            }
        };
        for (const auto kind : {minitun::daemon::ServerCredentialKind::psk,
                                minitun::daemon::ServerCredentialKind::ca_certificate,
                                minitun::daemon::ServerCredentialKind::client_certificate,
                                minitun::daemon::ServerCredentialKind::client_private_key}) {
            const auto reference = minitun::daemon::credential_reference(server, kind);
            if (!reference.has_value()) {
                auto removed =
                    minitun::daemon::cleanup_server_credential_kind(credentials, server.id, kind);
                if (!removed) {
                    return removed;
                }
                continue;
            }
            auto credential = credentials.get(*reference);
            if (credential) {
                auto removed = minitun::daemon::cleanup_server_credential_kind(
                    credentials, server.id, kind, std::string_view{*reference},
                    std::string_view{*reference});
                if (!removed) {
                    return removed;
                }
                continue;
            }
            if (credential.error().code() != minitun::common::ErrorCode::not_found) {
                return credential.error();
            }
            auto removed = minitun::daemon::cleanup_server_credential_kind(
                credentials, server.id, kind, std::string_view{*reference});
            if (!removed) {
                return removed;
            }
            clear_reference(kind);
            record_changed = true;
        }
        if (server.client_certificate_ref.has_value() !=
            server.client_private_key_ref.has_value()) {
            for (const auto kind : {minitun::daemon::ServerCredentialKind::client_certificate,
                                    minitun::daemon::ServerCredentialKind::client_private_key}) {
                const auto reference = minitun::daemon::credential_reference(server, kind);
                auto removed = minitun::daemon::cleanup_server_credential_kind(
                    credentials, server.id, kind,
                    reference.has_value() ? std::optional<std::string_view>{*reference}
                                          : std::nullopt);
                if (!removed) {
                    return removed;
                }
                clear_reference(kind);
            }
            record_changed = true;
        }
        if (record_changed) {
            if (!server.credential_ref.has_value()) {
                server.actual_state =
                    server.desired_state == minitun::storage::ServerDesiredState::enabled
                        ? minitun::storage::ServerActualState::not_authenticated
                        : minitun::storage::ServerActualState::disabled;
            }
            if (server.config_revision >=
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return minitun::common::Error{minitun::common::ErrorCode::resource_exhausted,
                                              "server configuration revision is exhausted"};
            }
            ++server.config_revision;
            server.last_error_code.reset();
            server.last_error_message.reset();
            server.updated_at_unix_ms =
                std::max(server.updated_at_unix_ms, minitun::common::unix_milliseconds_now());
            auto updated = repository.servers().update(server);
            if (!updated) {
                return updated;
            }
        }
    }
    return minitun::common::Result<void>::success();
}

int run_daemon_impl(const std::string& socket_path, const std::string& database_path,
                    const std::string& credentials_path,
                    minitun::daemon::ServerManagerOptions server_manager_options,
                    minitun::admin::ServerOptions admin_options,
                    const minitun::common::LogLevel log_level, const std::size_t io_threads) {
    const auto logging = minitun::common::initialize_logging({
        .logger_name = "minitund",
        .component = "daemon",
        .level = log_level,
    });
    if (!logging) {
        std::cerr << "minitund: failed to initialize logging: " << logging.error() << '\n';
        return kInternalErrorExitCode;
    }
    const LoggingLifetime logging_lifetime;

    auto repository = minitun::storage::StateRepository::open(database_path);
    if (!repository) {
        std::cerr << "minitund: failed to open state database: " << repository.error() << '\n';
        return repository.error().code() == minitun::common::ErrorCode::invalid_argument
                   ? kInvalidArgumentsExitCode
                   : kInternalErrorExitCode;
    }
    auto credentials = minitun::storage::SqliteCredentialStore::open(credentials_path);
    if (!credentials) {
        std::cerr << "minitund: failed to open credential database: " << credentials.error()
                  << '\n';
        return credentials.error().code() == minitun::common::ErrorCode::invalid_argument
                   ? kInvalidArgumentsExitCode
                   : kInternalErrorExitCode;
    }
    auto snapshot = (*repository)->recover();
    if (!snapshot) {
        std::cerr << "minitund: failed to recover persisted state: " << snapshot.error() << '\n';
        return kInternalErrorExitCode;
    }
    auto credential_state = validate_recovered_credentials(**repository, **credentials, *snapshot);
    if (!credential_state) {
        std::cerr << "minitund: failed to validate recovered credentials: "
                  << credential_state.error() << '\n';
        return kInternalErrorExitCode;
    }
    auto client_id = (*repository)->client_id();
    if (!client_id) {
        std::cerr << "minitund: failed to load the stable client identity: " << client_id.error()
                  << '\n';
        return kInternalErrorExitCode;
    }

    asio::io_context io_context;
    auto server_manager = minitun::daemon::ServerManager::create(
        io_context, **repository, **credentials, std::move(*client_id),
        std::move(server_manager_options));
    if (!server_manager) {
        std::cerr << "minitund: failed to configure remote server sessions: "
                  << server_manager.error() << '\n';
        return server_manager.error().code() == minitun::common::ErrorCode::invalid_argument
                   ? kInvalidArgumentsExitCode
                   : kInternalErrorExitCode;
    }

    minitun::daemon::ControlService control_service{
        **repository, **credentials,
        [manager = server_manager->get()] { manager->notify_changed(); },
        [manager = server_manager->get()] { return manager->metrics(); },
        [manager = server_manager->get()] {
            manager->reload();
            return minitun::common::Result<void>::success();
        }};
    auto dispatcher = std::make_shared<minitun::ipc::Dispatcher>();
    auto registered = control_service.register_handlers(*dispatcher);
    if (!registered) {
        std::cerr << "minitund: failed to register IPC methods: " << registered.error() << '\n';
        return kInternalErrorExitCode;
    }

    auto ipc_options = minitun::ipc::LocalServerOptions{};
    ipc_options.socket_path = socket_path;
    minitun::ipc::LocalServer ipc_server{io_context, dispatcher, std::move(ipc_options)};
    const auto ipc_ready = std::make_shared<std::atomic_bool>(false);
    std::unique_ptr<minitun::admin::Server> admin_server;
    if (!admin_options.listen_endpoint.empty()) {
        auto configured = minitun::admin::Server::create(
            io_context, std::move(admin_options),
            {.healthy =
                 [repository = repository->get(), credentials = credentials->get()] {
                     auto state = repository->diagnostics();
                     auto secrets = credentials->diagnostics();
                     return state && secrets && state->schema_valid && state->integrity_ok &&
                            secrets->schema_valid && secrets->integrity_ok;
                 },
             .ready =
                 [repository = repository->get(), credentials = credentials->get(), ipc_ready] {
                     auto state = repository->diagnostics();
                     auto secrets = credentials->diagnostics();
                     return ipc_ready->load(std::memory_order_relaxed) && state && secrets &&
                            state->schema_valid && state->integrity_ok && secrets->schema_valid &&
                            secrets->integrity_ok;
                 },
             .metrics = [manager =
                             server_manager->get()] { return daemon_metrics_text(*manager); }});
        if (!configured) {
            std::cerr << "minitund: failed to configure admin listener: " << configured.error()
                      << '\n';
            return kInvalidArgumentsExitCode;
        }
        admin_server = std::move(*configured);
    }
    asio::signal_set signals{io_context, SIGINT, SIGTERM, SIGHUP};
    auto signal_handler = std::make_shared<std::function<void(const asio::error_code&, int)>>();
    const std::weak_ptr weak_signal_handler = signal_handler;
    *signal_handler = [&ipc_server, &server_manager, &admin_server, &signals, ipc_ready,
                       weak_signal_handler](const asio::error_code& error,
                                            const int signal_number) {
        if (error) {
            return;
        }
        if (signal_number == SIGHUP) {
            (*server_manager)->reload();
            minitun::common::log_info("configuration reload requested");
            if (const auto handler = weak_signal_handler.lock()) {
                signals.async_wait(*handler);
            }
            return;
        }
        ipc_ready->store(false, std::memory_order_relaxed);
        ipc_server.stop();
        if (admin_server != nullptr) {
            admin_server->stop();
        }
        (*server_manager)->stop();
    };
    signals.async_wait(*signal_handler);

    auto started = ipc_server.start();
    if (!started) {
        std::cerr << "minitund: failed to start local IPC: " << started.error() << '\n';
        return started.error().code() == minitun::common::ErrorCode::invalid_argument
                   ? kInvalidArgumentsExitCode
                   : kInternalErrorExitCode;
    }
    ipc_ready->store(true, std::memory_order_relaxed);
    started = (*server_manager)->start();
    if (!started) {
        ipc_ready->store(false, std::memory_order_relaxed);
        ipc_server.stop();
        std::cerr << "minitund: failed to start remote server sessions: " << started.error()
                  << '\n';
        return kInternalErrorExitCode;
    }
    if (admin_server != nullptr) {
        started = admin_server->start();
        if (!started) {
            ipc_ready->store(false, std::memory_order_relaxed);
            (*server_manager)->stop();
            ipc_server.stop();
            std::cerr << "minitund: failed to start admin listener: " << started.error() << '\n';
            return kInternalErrorExitCode;
        }
    }

    minitun::common::log_info("local IPC service started");
    std::exception_ptr worker_failure;
    std::mutex worker_failure_mutex;
    std::vector<std::jthread> workers;
    workers.reserve(io_threads > 0U ? io_threads - 1U : 0U);
    for (std::size_t index = 1U; index < io_threads; ++index) {
        workers.emplace_back([&io_context, &worker_failure, &worker_failure_mutex] {
            try {
                io_context.run();
            } catch (...) {
                const std::scoped_lock lock{worker_failure_mutex};
                if (!worker_failure) {
                    worker_failure = std::current_exception();
                }
                io_context.stop();
            }
        });
    }
    try {
        io_context.run();
    } catch (...) {
        ipc_ready->store(false, std::memory_order_relaxed);
        io_context.stop();
        for (auto& worker : workers) {
            worker.join();
        }
        (*server_manager)->stop();
        if (admin_server != nullptr) {
            admin_server->stop();
        }
        ipc_server.stop();
        throw;
    }
    for (auto& worker : workers) {
        worker.join();
    }
    ipc_ready->store(false, std::memory_order_relaxed);
    (*server_manager)->stop();
    if (admin_server != nullptr) {
        admin_server->stop();
    }
    ipc_server.stop();
    {
        const std::scoped_lock lock{worker_failure_mutex};
        if (worker_failure) {
            std::rethrow_exception(worker_failure);
        }
    }

    minitun::common::log_info("local IPC service stopped");
    return kSuccessExitCode;
}

int run_daemon(const std::string& socket_path, const std::string& database_path,
               const std::string& credentials_path,
               minitun::daemon::ServerManagerOptions server_manager_options,
               minitun::admin::ServerOptions admin_options,
               const minitun::common::LogLevel log_level, const std::size_t io_threads) noexcept {
    try {
        return run_daemon_impl(socket_path, database_path, credentials_path,
                               std::move(server_manager_options), std::move(admin_options),
                               log_level, io_threads);
    } catch (...) {
        minitun::common::shutdown_logging();
        std::cerr << "minitund: unexpected daemon startup or event-loop failure\n";
        return kInternalErrorExitCode;
    }
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{
        "MiniTun client daemon",
        "minitund",
    };

    const std::string version_info = minitun::common::format_version_info("minitund");
    app.set_version_flag("--version", version_info, "Show version information");

    std::string socket_path{minitun::ipc::kDefaultSocketPath};
    app.add_option("--socket", socket_path, "Local Unix socket path")->capture_default_str();

    std::string database_path{minitun::storage::kDefaultDatabasePath};
    app.add_option("--database", database_path, "Persistent state database path")
        ->capture_default_str();

    std::string credentials_path{minitun::storage::kDefaultCredentialsPath};
    app.add_option("--credentials", credentials_path, "Credential database path")
        ->capture_default_str();

    std::string admin_listen;
    std::string admin_token_file;
    app.add_option("--admin-listen", admin_listen,
                   "Optional numeric host:port for health and metrics");
    app.add_option("--admin-token-file", admin_token_file,
                   "Private Bearer token file required for non-loopback admin listeners")
        ->check(CLI::ExistingFile);

    std::string tls_ca_path;
    app.add_option("--tls-ca", tls_ca_path, "PEM CA certificate used to verify remote servers")
        ->check(CLI::ExistingFile);

    bool insecure_skip_verify = false;
    app.add_flag("--insecure-skip-verify", insecure_skip_verify,
                 "Development only: disable remote TLS certificate verification");

    int relay_idle_timeout_seconds = 300;
    app.add_option("--relay-idle-timeout", relay_idle_timeout_seconds,
                   "Relay inactivity timeout in seconds")
        ->check(CLI::Range(1, 86'400))
        ->capture_default_str();

    int shutdown_timeout_seconds = 10;
    app.add_option("--shutdown-timeout", shutdown_timeout_seconds,
                   "Maximum graceful relay drain time in seconds")
        ->check(CLI::Range(1, 300))
        ->capture_default_str();

    std::size_t max_idle_workers_per_server = 32U;
    app.add_option("--max-idle-workers-per-server", max_idle_workers_per_server,
                   "Maximum idle Workers retained for each remote server")
        ->check(CLI::Range(1U, 128U))
        ->capture_default_str();

    std::size_t max_total_idle_workers = 128U;
    app.add_option("--max-total-idle-workers", max_total_idle_workers,
                   "Maximum idle Workers retained across all remote servers")
        ->check(CLI::Range(1U, 4'096U))
        ->capture_default_str();

    std::size_t max_total_connections = 10'000U;
    app.add_option("--max-total-connections", max_total_connections,
                   "Maximum Worker and relay connections across all remote servers")
        ->check(CLI::Range(1U, 100'000U))
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
        std::cerr << "minitund: " << log_level.error() << '\n';
        return kInvalidArgumentsExitCode;
    }
    if (insecure_skip_verify) {
        std::cerr << "WARNING: --insecure-skip-verify disables TLS peer verification; "
                     "use development environments only.\n";
    }

    static_cast<void>(foreground);
    return run_daemon(
        socket_path, database_path, credentials_path,
        minitun::daemon::ServerManagerOptions{
            .ca_certificate_path = std::move(tls_ca_path),
            .insecure_skip_verify = insecure_skip_verify,
            .relay_inactivity_timeout = std::chrono::seconds{relay_idle_timeout_seconds},
            .graceful_shutdown_timeout = std::chrono::seconds{shutdown_timeout_seconds},
            .max_idle_workers_per_server = max_idle_workers_per_server,
            .max_total_idle_workers = max_total_idle_workers,
            .max_total_connections = max_total_connections,
        },
        minitun::admin::ServerOptions{.listen_endpoint = std::move(admin_listen),
                                      .token_file = std::move(admin_token_file)},
        *log_level, io_threads);
}
