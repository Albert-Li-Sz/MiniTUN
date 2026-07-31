#include <CLI/CLI.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#include <minitun/common/version.hpp>

namespace {

constexpr int kSuccessExitCode = EXIT_SUCCESS;
constexpr int kInvalidArgumentsExitCode = 2;

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

} // namespace

int main(int argc, char** argv) {
    CLI::App app{
        "MiniTun command-line client",
        "minitun",
    };

    const std::string version_info = minitun::common::format_version_info("minitun");
    app.set_version_flag("--version", version_info, "Show version information");

    CLI::App* const version_command = app.add_subcommand("version", "Show version information");
    CLI::App* const help_command = app.add_subcommand("help", "Show this help message");
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

    if (help_command->parsed() || argc == 1) {
        std::cout << app.help();
    }

    return kSuccessExitCode;
}
