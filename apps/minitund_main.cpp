#include <CLI/CLI.hpp>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <minitun/common/error.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/time.hpp>
#include <minitun/common/version.hpp>
#include <minitun/daemon/control_service.hpp>
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

[[nodiscard]] std::string credential_key(const minitun::storage::ServerRecord& server) {
    std::string key{"server/"};
    key.append(server.id.str());
    return key;
}

[[nodiscard]] minitun::common::Result<void>
remove_credential_if_present(minitun::storage::CredentialStore& credentials,
                             const std::string_view key) {
    auto credential = credentials.get(key);
    if (credential) {
        return credentials.remove(key);
    }
    if (credential.error().code() == minitun::common::ErrorCode::not_found) {
        return minitun::common::Result<void>::success();
    }
    return credential.error();
}

[[nodiscard]] minitun::common::Result<void>
validate_recovered_credentials(minitun::storage::StateRepository& repository,
                               minitun::storage::CredentialStore& credentials,
                               const minitun::storage::RecoverySnapshot& snapshot) {
    for (auto server : snapshot.servers) {
        const std::string stable_key = credential_key(server);
        if (!server.credential_ref.has_value()) {
            auto removed = remove_credential_if_present(credentials, stable_key);
            if (!removed) {
                return removed;
            }
            continue;
        }
        if (server.desired_state == minitun::storage::ServerDesiredState::removed) {
            auto removed = credentials.remove(*server.credential_ref);
            if (!removed) {
                return removed;
            }
            if (*server.credential_ref != stable_key) {
                removed = remove_credential_if_present(credentials, stable_key);
                if (!removed) {
                    return removed;
                }
            }
            continue;
        }

        auto credential = credentials.get(*server.credential_ref);
        if (credential) {
            if (*server.credential_ref != stable_key) {
                auto removed = remove_credential_if_present(credentials, stable_key);
                if (!removed) {
                    return removed;
                }
            }
            continue;
        }
        if (credential.error().code() != minitun::common::ErrorCode::not_found) {
            return credential.error();
        }
        if (*server.credential_ref != stable_key) {
            auto removed = remove_credential_if_present(credentials, stable_key);
            if (!removed) {
                return removed;
            }
        }
        server.credential_ref.reset();
        server.actual_state = server.desired_state == minitun::storage::ServerDesiredState::enabled
                                  ? minitun::storage::ServerActualState::not_authenticated
                                  : minitun::storage::ServerActualState::disabled;
        server.last_error_code.reset();
        server.last_error_message.reset();
        server.updated_at_unix_ms =
            std::max(server.updated_at_unix_ms, minitun::common::unix_milliseconds_now());
        auto updated = repository.servers().update(server);
        if (!updated) {
            return updated;
        }
    }
    return minitun::common::Result<void>::success();
}

int run_daemon_impl(const std::string& socket_path, const std::string& database_path,
                    const std::string& credentials_path,
                    minitun::daemon::ServerManagerOptions server_manager_options,
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

    minitun::daemon::ControlService control_service{**repository, **credentials};
    auto dispatcher = std::make_shared<minitun::ipc::Dispatcher>();
    auto registered = control_service.register_handlers(*dispatcher);
    if (!registered) {
        std::cerr << "minitund: failed to register IPC methods: " << registered.error() << '\n';
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

    minitun::ipc::LocalServer ipc_server{
        io_context,
        dispatcher,
        minitun::ipc::LocalServerOptions{.socket_path = socket_path},
    };
    asio::signal_set signals{io_context, SIGINT, SIGTERM};
    signals.async_wait([&ipc_server, &io_context](const asio::error_code& error, int) {
        if (!error) {
            ipc_server.stop();
            io_context.stop();
        }
    });

    auto started = ipc_server.start();
    if (!started) {
        std::cerr << "minitund: failed to start local IPC: " << started.error() << '\n';
        return started.error().code() == minitun::common::ErrorCode::invalid_argument
                   ? kInvalidArgumentsExitCode
                   : kInternalErrorExitCode;
    }
    started = (*server_manager)->start();
    if (!started) {
        ipc_server.stop();
        std::cerr << "minitund: failed to start remote server sessions: " << started.error()
                  << '\n';
        return kInternalErrorExitCode;
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
        io_context.stop();
        for (auto& worker : workers) {
            worker.join();
        }
        (*server_manager)->stop();
        ipc_server.stop();
        throw;
    }
    for (auto& worker : workers) {
        worker.join();
    }
    (*server_manager)->stop();
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
               const minitun::common::LogLevel log_level, const std::size_t io_threads) noexcept {
    try {
        return run_daemon_impl(socket_path, database_path, credentials_path,
                               std::move(server_manager_options), log_level, io_threads);
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

    std::string tls_ca_path;
    app.add_option("--tls-ca", tls_ca_path, "PEM CA certificate used to verify remote servers")
        ->check(CLI::ExistingFile);

    bool insecure_skip_verify = false;
    app.add_flag("--insecure-skip-verify", insecure_skip_verify,
                 "Development only: disable remote TLS certificate verification");

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
    return run_daemon(socket_path, database_path, credentials_path,
                      minitun::daemon::ServerManagerOptions{
                          .ca_certificate_path = std::move(tls_ca_path),
                          .insecure_skip_verify = insecure_skip_verify,
                      },
                      *log_level, io_threads);
}
