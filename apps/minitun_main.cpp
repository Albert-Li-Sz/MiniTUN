#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/version.hpp>
#include <minitun/ipc/local_client.hpp>
#include <minitun/ipc/protocol.hpp>

namespace {

constexpr int kSuccessExitCode = EXIT_SUCCESS;
constexpr int kInvalidArgumentsExitCode = 2;
constexpr int kDaemonUnavailableExitCode = 3;
constexpr int kInternalErrorExitCode = 10;

[[nodiscard]] int request_failure_exit_code(const minitun::common::ErrorCode code) noexcept {
    switch (code) {
    case minitun::common::ErrorCode::invalid_argument:
        return kInvalidArgumentsExitCode;
    case minitun::common::ErrorCode::permission_denied:
    case minitun::common::ErrorCode::connection_failed:
    case minitun::common::ErrorCode::connection_timeout:
    case minitun::common::ErrorCode::ipc_error:
        return kDaemonUnavailableExitCode;
    default:
        return kInternalErrorExitCode;
    }
}

int parse_exit_code(CLI::App& app, const CLI::ParseError& error) {
    const int cli_exit_code = app.exit(error);
    return cli_exit_code == kSuccessExitCode ? kSuccessExitCode : kInvalidArgumentsExitCode;
}

void print_line_terminated(const std::string& text) {
    std::cout << text;
    if (text.empty() || text.back() != '\n') {
        std::cout << '\n';
    }
}

int print_daemon_status(const std::string& socket_path) {
    auto request_id = minitun::common::Id::generate(minitun::common::IdKind::request);
    if (!request_id) {
        std::cerr << "minitun: failed to create an IPC request ID\n";
        return kInternalErrorExitCode;
    }

    minitun::ipc::Request request{
        minitun::ipc::kProtocolVersion,
        std::move(request_id).value(),
        "daemon.status",
        minitun::ipc::Json::object(),
    };
    minitun::ipc::LocalClient client{
        minitun::ipc::LocalClientOptions{.socket_path = socket_path},
    };
    auto response = client.request(request);
    if (!response) {
        const int exit_code = request_failure_exit_code(response.error().code());
        const char* const category = exit_code == kInvalidArgumentsExitCode ? "invalid argument"
                                     : exit_code == kDaemonUnavailableExitCode
                                         ? "daemon unavailable"
                                         : "IPC failure";
        std::cerr << "minitun: " << category << ": " << response.error() << '\n';
        return exit_code;
    }
    if (!response->ok()) {
        const minitun::common::Error* const error = response->error();
        if (error == nullptr) {
            std::cerr << "minitun: daemon returned an invalid error response\n";
        } else {
            std::cerr << "minitun: daemon request failed: " << *error << '\n';
        }
        return error == nullptr ? kInternalErrorExitCode : request_failure_exit_code(error->code());
    }

    const minitun::ipc::Json* const result = response->result();
    if (result == nullptr || !result->is_object()) {
        std::cerr << "minitun: daemon returned an invalid status result\n";
        return kInternalErrorExitCode;
    }
    const auto state = result->find("state");
    const auto ipc_version = result->find("ipc_version");
    if (result->size() != 2U || state == result->end() || !state->is_string() ||
        state->get_ref<const std::string&>() != "running" || ipc_version == result->end() ||
        (!ipc_version->is_number_integer() && !ipc_version->is_number_unsigned())) {
        std::cerr << "minitun: daemon returned an invalid status result\n";
        return kInternalErrorExitCode;
    }

    const bool version_matches =
        ipc_version->is_number_unsigned()
            ? ipc_version->get<std::uint64_t>() == minitun::ipc::kProtocolVersion
            : ipc_version->get<std::int64_t>() == minitun::ipc::kProtocolVersion;
    if (!version_matches) {
        std::cerr << "minitun: daemon reported an unsupported IPC version\n";
        return kInternalErrorExitCode;
    }

    std::cout << "Daemon status:\n"
              << "  State        running\n"
              << "  IPC version  " << minitun::ipc::kProtocolVersion << '\n';
    return kSuccessExitCode;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{
        "MiniTun command-line client",
        "minitun",
    };

    const std::string version_info = minitun::common::format_version_info("minitun");
    app.set_version_flag("--version", version_info, "Show version information");

    std::string socket_path{minitun::ipc::kDefaultSocketPath};
    app.add_option("--socket", socket_path, "Local minitund Unix socket path")
        ->capture_default_str();

    CLI::App* const version_command = app.add_subcommand("version", "Show version information");
    CLI::App* const help_command = app.add_subcommand("help", "Show this help message");
    CLI::App* const daemon_command = app.add_subcommand("daemon", "Inspect the local daemon");
    CLI::App* const daemon_status_command =
        daemon_command->add_subcommand("status", "Show local daemon status");
    daemon_command->require_subcommand(1);
    app.require_subcommand(0, 1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return parse_exit_code(app, error);
    }

    if (version_command->parsed()) {
        print_line_terminated(version_info);
        return kSuccessExitCode;
    }

    if (daemon_status_command->parsed()) {
        try {
            return print_daemon_status(socket_path);
        } catch (...) {
            std::cerr << "minitun: unexpected local IPC failure\n";
            return kInternalErrorExitCode;
        }
    }

    if (help_command->parsed() || argc == 1) {
        std::cout << app.help();
    }

    return kSuccessExitCode;
}
