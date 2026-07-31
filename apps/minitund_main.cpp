#include <CLI/CLI.hpp>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <minitun/common/error.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/version.hpp>
#include <minitun/ipc/dispatcher.hpp>
#include <minitun/ipc/local_server.hpp>
#include <minitun/ipc/protocol.hpp>

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

int run_daemon_impl(const std::string& socket_path) {
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

    auto dispatcher = std::make_shared<minitun::ipc::Dispatcher>();
    auto registered = dispatcher->register_handler(
        "daemon.status",
        [](const minitun::ipc::Request& request) -> minitun::common::Result<minitun::ipc::Json> {
            if (!request.params.empty()) {
                return minitun::common::Result<minitun::ipc::Json>::failure(
                    minitun::common::ErrorCode::invalid_argument,
                    "daemon.status does not accept parameters");
            }
            return minitun::ipc::Json{
                {"state", "running"},
                {"ipc_version", minitun::ipc::kProtocolVersion},
            };
        });
    if (!registered) {
        std::cerr << "minitund: failed to register daemon.status: " << registered.error() << '\n';
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

int run_daemon(const std::string& socket_path) noexcept {
    try {
        return run_daemon_impl(socket_path);
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

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return parse_exit_code(app, error);
    }

    return run_daemon(socket_path);
}
