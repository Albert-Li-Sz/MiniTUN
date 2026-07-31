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

} // namespace

int main(int argc, char** argv) {
    CLI::App app{
        "MiniTun client daemon",
        "minitund",
    };

    const std::string version_info = minitun::common::format_version_info("minitund");
    app.set_version_flag("--version", version_info, "Show version information");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return parse_exit_code(app, error);
    }

    if (argc == 1) {
        std::cout << app.help();
    }

    return kSuccessExitCode;
}
