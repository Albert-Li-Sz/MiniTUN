#include <CLI/CLI.hpp>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <minitun/common/error.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/time.hpp>
#include <minitun/common/version.hpp>
#include <minitun/daemon/control_service.hpp>
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
                    const std::string& credentials_path) {
    const auto logging = minitun::common::initialize_logging({
        .logger_name = "minitund",
        .component = "daemon",
        .level = minitun::common::LogLevel::info,
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

    minitun::daemon::ControlService control_service{**repository, **credentials};
    auto dispatcher = std::make_shared<minitun::ipc::Dispatcher>();
    auto registered = control_service.register_handlers(*dispatcher);
    if (!registered) {
        std::cerr << "minitund: failed to register IPC methods: " << registered.error() << '\n';
        return kInternalErrorExitCode;
    }

    asio::io_context io_context;
    minitun::ipc::LocalServer server{
        io_context,
        dispatcher,
        minitun::ipc::LocalServerOptions{.socket_path = socket_path},
    };
    asio::signal_set signals{io_context, SIGINT, SIGTERM};
    signals.async_wait([&server, &io_context](const asio::error_code& error, int) {
        if (!error) {
            server.stop();
            io_context.stop();
        }
    });

    auto started = server.start();
    if (!started) {
        std::cerr << "minitund: failed to start local IPC: " << started.error() << '\n';
        return started.error().code() == minitun::common::ErrorCode::invalid_argument
                   ? kInvalidArgumentsExitCode
                   : kInternalErrorExitCode;
    }

    minitun::common::log_info("local IPC service started");
    std::exception_ptr worker_failure;
    std::jthread io_worker{[&io_context, &worker_failure] {
        try {
            io_context.run();
        } catch (...) {
            worker_failure = std::current_exception();
            io_context.stop();
        }
    }};
    try {
        io_context.run();
    } catch (...) {
        io_context.stop();
        io_worker.join();
        throw;
    }
    io_worker.join();
    if (worker_failure) {
        std::rethrow_exception(worker_failure);
    }

    server.stop();
    minitun::common::log_info("local IPC service stopped");
    return kSuccessExitCode;
}

int run_daemon(const std::string& socket_path, const std::string& database_path,
               const std::string& credentials_path) noexcept {
    try {
        return run_daemon_impl(socket_path, database_path, credentials_path);
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

    bool foreground = false;
    app.add_flag("--foreground", foreground, "Run in the foreground");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return parse_exit_code(app, error);
    }

    static_cast<void>(foreground);
    return run_daemon(socket_path, database_path, credentials_path);
}
