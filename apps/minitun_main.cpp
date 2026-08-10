#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/version.hpp>
#include <minitun/ipc/local_client.hpp>
#include <minitun/ipc/protocol.hpp>

namespace {

constexpr int kSuccessExitCode = EXIT_SUCCESS;
constexpr int kInvalidArgumentsExitCode = 2;
constexpr int kDaemonUnavailableExitCode = 3;
constexpr int kAuthenticationFailureExitCode = 4;
constexpr int kRemoteFailureExitCode = 5;
constexpr int kInternalErrorExitCode = 10;
constexpr std::size_t kMaxCredentialBytes = 64U * 1024U;

enum class FailureOrigin : std::uint8_t {
    local_transport,
    daemon,
};

volatile std::sig_atomic_t terminal_signal = 0;

extern "C" void terminal_signal_handler(const int signal_number) {
    terminal_signal = signal_number;
}

class TerminalInputGuard final {
  public:
    [[nodiscard]] minitun::common::Result<void> start() {
        if (::tcgetattr(STDIN_FILENO, &original_terminal_) != 0) {
            return minitun::common::Result<void>::failure(
                minitun::common::ErrorCode::invalid_argument,
                "standard input is not an interactive terminal");
        }

        struct sigaction action{};
        action.sa_handler = terminal_signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        terminal_signal = 0;
        for (std::size_t index = 0; index < signals_.size(); ++index) {
            if (::sigaction(signals_[index], &action, &original_actions_[index]) != 0) {
                restore_signals();
                return minitun::common::Result<void>::failure(
                    minitun::common::ErrorCode::internal_error,
                    "failed to prepare secure terminal input");
            }
            ++installed_actions_;
        }

        termios hidden = original_terminal_;
        hidden.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL));
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
            restore_signals();
            return minitun::common::Result<void>::failure(
                minitun::common::ErrorCode::internal_error, "failed to disable terminal echo");
        }
        terminal_active_ = true;
        return minitun::common::Result<void>::success();
    }

    ~TerminalInputGuard() noexcept { restore(); }

    TerminalInputGuard() = default;
    TerminalInputGuard(const TerminalInputGuard&) = delete;
    TerminalInputGuard& operator=(const TerminalInputGuard&) = delete;

    void restore() noexcept {
        if (terminal_active_) {
            static_cast<void>(::tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal_));
            terminal_active_ = false;
        }
        restore_signals();
    }

  private:
    void restore_signals() noexcept {
        while (installed_actions_ != 0U) {
            --installed_actions_;
            static_cast<void>(::sigaction(signals_[installed_actions_],
                                          &original_actions_[installed_actions_], nullptr));
        }
    }

    static constexpr std::array<int, 5> signals_{SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP};
    std::array<struct sigaction, signals_.size()> original_actions_{};
    termios original_terminal_{};
    std::size_t installed_actions_{0U};
    bool terminal_active_{false};
};

void secure_erase_json(minitun::ipc::Json& value) noexcept {
    try {
        if (value.is_string()) {
            auto& text = value.get_ref<std::string&>();
            minitun::common::secure_erase_memory(text.data(), text.size());
            text.clear();
            return;
        }
        if (value.is_array() || value.is_object()) {
            for (auto& child : value) {
                secure_erase_json(child);
            }
        }
    } catch (...) {
    }
}

class JsonScrubber final {
  public:
    explicit JsonScrubber(minitun::ipc::Json& value) noexcept : value_(value) {}
    ~JsonScrubber() noexcept { secure_erase_json(value_); }

    JsonScrubber(const JsonScrubber&) = delete;
    JsonScrubber& operator=(const JsonScrubber&) = delete;

  private:
    minitun::ipc::Json& value_;
};

class StringScrubber final {
  public:
    explicit StringScrubber(std::string& value) noexcept : value_(value) {}
    ~StringScrubber() noexcept {
        minitun::common::secure_erase_memory(value_.data(), value_.size());
        value_.clear();
    }

    StringScrubber(const StringScrubber&) = delete;
    StringScrubber& operator=(const StringScrubber&) = delete;

  private:
    std::string& value_;
};

[[nodiscard]] int daemon_error_exit_code(const minitun::common::ErrorCode code) noexcept {
    switch (code) {
    case minitun::common::ErrorCode::invalid_argument:
    case minitun::common::ErrorCode::not_found:
    case minitun::common::ErrorCode::already_exists:
        return kInvalidArgumentsExitCode;
    case minitun::common::ErrorCode::not_authenticated:
    case minitun::common::ErrorCode::authentication_failed:
        return kAuthenticationFailureExitCode;
    case minitun::common::ErrorCode::connection_failed:
    case minitun::common::ErrorCode::connection_timeout:
    case minitun::common::ErrorCode::remote_port_in_use:
    case minitun::common::ErrorCode::local_connect_failed:
    case minitun::common::ErrorCode::tls_error:
        return kRemoteFailureExitCode;
    default:
        return kInternalErrorExitCode;
    }
}

[[nodiscard]] int local_error_exit_code(const minitun::common::ErrorCode code) noexcept {
    switch (code) {
    case minitun::common::ErrorCode::invalid_argument:
        return kInvalidArgumentsExitCode;
    case minitun::common::ErrorCode::permission_denied:
    case minitun::common::ErrorCode::connection_failed:
    case minitun::common::ErrorCode::connection_timeout:
    case minitun::common::ErrorCode::ipc_error:
    case minitun::common::ErrorCode::not_found:
        return kDaemonUnavailableExitCode;
    default:
        return kInternalErrorExitCode;
    }
}

[[nodiscard]] int report_error(const minitun::common::Error& error, const FailureOrigin origin) {
    const int exit_code = origin == FailureOrigin::local_transport
                              ? local_error_exit_code(error.code())
                              : daemon_error_exit_code(error.code());
    const char* category = "internal failure";
    if (exit_code == kInvalidArgumentsExitCode) {
        category = "invalid argument";
    } else if (exit_code == kDaemonUnavailableExitCode) {
        category = "daemon unavailable";
    } else if (exit_code == kAuthenticationFailureExitCode) {
        category = "authentication failure";
    } else if (exit_code == kRemoteFailureExitCode) {
        category = "remote failure";
    }
    std::cerr << "minitun: " << category << ": " << error << '\n';
    return exit_code;
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

[[nodiscard]] minitun::common::Result<minitun::ipc::Json>
request_daemon(const std::string& socket_path, std::string method, minitun::ipc::Json params,
               FailureOrigin& origin) {
    origin = FailureOrigin::local_transport;
    auto request_id = minitun::common::Id::generate(minitun::common::IdKind::request);
    if (!request_id) {
        secure_erase_json(params);
        return request_id.error();
    }
    minitun::ipc::Request request{
        minitun::ipc::kProtocolVersion,
        std::move(*request_id),
        std::move(method),
        std::move(params),
    };
    const JsonScrubber request_scrubber{request.params};
    minitun::ipc::LocalClient client{
        minitun::ipc::LocalClientOptions{.socket_path = socket_path},
    };
    auto response = client.request(request);
    if (!response) {
        return response.error();
    }
    if (!response->ok()) {
        origin = FailureOrigin::daemon;
        const minitun::common::Error* const error = response->error();
        if (error == nullptr) {
            return minitun::common::Error{minitun::common::ErrorCode::protocol_error,
                                          "daemon returned an invalid error response"};
        }
        return *error;
    }
    const minitun::ipc::Json* const result = response->result();
    if (result == nullptr || !result->is_object()) {
        return minitun::common::Error{minitun::common::ErrorCode::protocol_error,
                                      "daemon returned an invalid result"};
    }
    return *result;
}

[[nodiscard]] bool nullable_string(const minitun::ipc::Json& value) {
    return value.is_null() || value.is_string();
}

[[nodiscard]] bool nonnegative_integer(const minitun::ipc::Json& value) {
    if (value.is_number_unsigned()) {
        return true;
    }
    return value.is_number_integer() && value.get<std::int64_t>() >= 0;
}

[[nodiscard]] std::optional<std::uint64_t> json_count(const minitun::ipc::Json& object,
                                                      const std::string_view field) {
    const auto value = object.find(field);
    if (value == object.end() || !nonnegative_integer(*value)) {
        return std::nullopt;
    }
    return value->is_number_unsigned() ? std::optional<std::uint64_t>{value->get<std::uint64_t>()}
                                       : std::optional<std::uint64_t>{static_cast<std::uint64_t>(
                                             value->get<std::int64_t>())};
}

[[nodiscard]] bool valid_server(const minitun::ipc::Json& server) {
    if (!server.is_object()) {
        return false;
    }
    const auto id = server.find("id");
    const auto name = server.find("name");
    const auto endpoint = server.find("endpoint");
    const auto actual_state = server.find("actual_state");
    const auto desired_state = server.find("desired_state");
    const auto credential_configured = server.find("credential_configured");
    const auto latency = server.find("latency_ms");
    return id != server.end() && id->is_string() && name != server.end() &&
           nullable_string(*name) && endpoint != server.end() && endpoint->is_string() &&
           actual_state != server.end() && actual_state->is_string() &&
           desired_state != server.end() && desired_state->is_string() &&
           credential_configured != server.end() && credential_configured->is_boolean() &&
           latency != server.end() && (latency->is_null() || nonnegative_integer(*latency)) &&
           json_count(server, "tunnel_count").has_value();
}

[[nodiscard]] bool valid_tunnel(const minitun::ipc::Json& tunnel) {
    if (!tunnel.is_object()) {
        return false;
    }
    const auto id = tunnel.find("id");
    const auto name = tunnel.find("name");
    const auto server_id = tunnel.find("server_id");
    const auto server_name = tunnel.find("server_name");
    const auto server_actual_state = tunnel.find("server_actual_state");
    const auto local = tunnel.find("local_endpoint");
    const auto remote = tunnel.find("remote_endpoint");
    const auto actual_state = tunnel.find("actual_state");
    const auto desired_state = tunnel.find("desired_state");
    const auto pending_reason = tunnel.find("pending_reason");
    const auto last_synced_at = tunnel.find("last_synced_at");
    return id != tunnel.end() && id->is_string() && name != tunnel.end() &&
           nullable_string(*name) && server_id != tunnel.end() && server_id->is_string() &&
           server_name != tunnel.end() && nullable_string(*server_name) && local != tunnel.end() &&
           local->is_string() && remote != tunnel.end() && remote->is_string() &&
           actual_state != tunnel.end() && actual_state->is_string() &&
           desired_state != tunnel.end() && desired_state->is_string() &&
           server_actual_state != tunnel.end() && nullable_string(*server_actual_state) &&
           pending_reason != tunnel.end() && nullable_string(*pending_reason) &&
           last_synced_at != tunnel.end() &&
           (last_synced_at->is_null() || nonnegative_integer(*last_synced_at));
}

[[nodiscard]] std::string nullable_text(const minitun::ipc::Json& object,
                                        const std::string_view field) {
    const auto value = object.find(field);
    return value != object.end() && value->is_string() ? value->get<std::string>() : "-";
}

[[nodiscard]] std::string tunnel_server_label(const minitun::ipc::Json& tunnel) {
    const auto name = tunnel.find("server_name");
    return name != tunnel.end() && name->is_string() ? name->get<std::string>()
                                                     : tunnel.at("server_id").get<std::string>();
}

void print_table(const std::vector<std::vector<std::string>>& rows) {
    if (rows.empty()) {
        return;
    }
    std::vector<std::size_t> widths(rows.front().size(), 0U);
    for (const auto& row : rows) {
        for (std::size_t column = 0; column < row.size(); ++column) {
            widths[column] = std::max(widths[column], row[column].size());
        }
    }
    for (const auto& row : rows) {
        for (std::size_t column = 0; column < row.size(); ++column) {
            std::cout << std::left << std::setw(static_cast<int>(widths[column] + 2U))
                      << row[column];
        }
        std::cout << '\n';
    }
}

[[nodiscard]] bool print_server_summary(const minitun::ipc::Json& result,
                                        const std::string_view title) {
    const auto server = result.find("server");
    if (server == result.end() || !valid_server(*server)) {
        return false;
    }
    std::cout << title << ":\n"
              << "  ID        " << server->at("id").get<std::string>() << '\n'
              << "  Name      " << nullable_text(*server, "name") << '\n'
              << "  Endpoint  " << server->at("endpoint").get<std::string>() << '\n'
              << "  Status    " << server->at("actual_state").get<std::string>() << '\n';
    return true;
}

[[nodiscard]] bool print_server_list(const minitun::ipc::Json& result, const bool json_output) {
    const auto servers = result.find("servers");
    if (servers == result.end() || !servers->is_array()) {
        return false;
    }
    for (const auto& server : *servers) {
        if (!valid_server(server)) {
            return false;
        }
    }
    if (json_output) {
        std::cout << servers->dump(2) << '\n';
        return true;
    }
    std::vector<std::vector<std::string>> rows{
        {"ID", "NAME", "ENDPOINT", "STATUS", "LATENCY", "TUNNELS"}};
    for (const auto& server : *servers) {
        const auto latency = server.find("latency_ms");
        const std::string latency_text =
            latency->is_null() ? "-" : std::to_string(*json_count(server, "latency_ms")) + " ms";
        rows.push_back({server.at("id").get<std::string>(), nullable_text(server, "name"),
                        server.at("endpoint").get<std::string>(),
                        server.at("actual_state").get<std::string>(), latency_text,
                        std::to_string(*json_count(server, "tunnel_count"))});
    }
    print_table(rows);
    return true;
}

[[nodiscard]] bool print_server_inspect(const minitun::ipc::Json& result, const bool json_output) {
    const auto server = result.find("server");
    if (server == result.end() || !valid_server(*server)) {
        return false;
    }
    if (json_output) {
        std::cout << server->dump(2) << '\n';
        return true;
    }
    const auto latency = server->find("latency_ms");
    const std::string latency_text =
        latency->is_null() ? "-" : std::to_string(*json_count(*server, "latency_ms")) + " ms";
    std::cout << "Server:\n"
              << "  ID             " << server->at("id").get<std::string>() << '\n'
              << "  Name           " << nullable_text(*server, "name") << '\n'
              << "  Endpoint       " << server->at("endpoint").get<std::string>() << '\n'
              << "  Desired state  " << server->at("desired_state").get<std::string>() << '\n'
              << "  Actual state   " << server->at("actual_state").get<std::string>() << '\n'
              << "  Auth           "
              << (server->at("credential_configured").get<bool>() ? "configured" : "not configured")
              << '\n'
              << "  Latency        " << latency_text << '\n'
              << "  Tunnels        " << *json_count(*server, "tunnel_count") << '\n';
    return true;
}

[[nodiscard]] bool print_tunnel_summary(const minitun::ipc::Json& result,
                                        const std::string_view title) {
    const auto tunnel = result.find("tunnel");
    if (tunnel == result.end() || !valid_tunnel(*tunnel)) {
        return false;
    }
    std::cout << title << ":\n"
              << "  ID      " << tunnel->at("id").get<std::string>() << '\n'
              << "  Name    " << nullable_text(*tunnel, "name") << '\n'
              << "  Server  " << tunnel_server_label(*tunnel) << '\n'
              << "  Local   " << tunnel->at("local_endpoint").get<std::string>() << '\n'
              << "  Remote  " << tunnel->at("remote_endpoint").get<std::string>() << '\n'
              << "  Status  " << tunnel->at("actual_state").get<std::string>() << '\n';
    return true;
}

[[nodiscard]] bool print_tunnel_list(const minitun::ipc::Json& result, const bool json_output) {
    const auto tunnels = result.find("tunnels");
    if (tunnels == result.end() || !tunnels->is_array()) {
        return false;
    }
    for (const auto& tunnel : *tunnels) {
        if (!valid_tunnel(tunnel)) {
            return false;
        }
    }
    if (json_output) {
        std::cout << tunnels->dump(2) << '\n';
        return true;
    }
    std::vector<std::vector<std::string>> rows{
        {"ID", "NAME", "SERVER", "LOCAL", "REMOTE", "STATUS"}};
    for (const auto& tunnel : *tunnels) {
        rows.push_back({tunnel.at("id").get<std::string>(), nullable_text(tunnel, "name"),
                        tunnel_server_label(tunnel), tunnel.at("local_endpoint").get<std::string>(),
                        tunnel.at("remote_endpoint").get<std::string>(),
                        tunnel.at("actual_state").get<std::string>()});
    }
    print_table(rows);
    return true;
}

[[nodiscard]] bool print_tunnel_inspect(const minitun::ipc::Json& result, const bool json_output) {
    const auto tunnel = result.find("tunnel");
    if (tunnel == result.end() || !valid_tunnel(*tunnel)) {
        return false;
    }
    if (json_output) {
        std::cout << tunnel->dump(2) << '\n';
        return true;
    }
    const auto last_synced_at = json_count(*tunnel, "last_synced_at");
    std::cout << "Tunnel:\n"
              << "  ID             " << tunnel->at("id").get<std::string>() << '\n'
              << "  Name           " << nullable_text(*tunnel, "name") << '\n'
              << "  Server         " << tunnel_server_label(*tunnel) << '\n'
              << "  Local          " << tunnel->at("local_endpoint").get<std::string>() << '\n'
              << "  Remote         " << tunnel->at("remote_endpoint").get<std::string>() << '\n'
              << "  Desired state  " << tunnel->at("desired_state").get<std::string>() << '\n'
              << "  Actual state   " << tunnel->at("actual_state").get<std::string>() << '\n'
              << "  Server state   " << nullable_text(*tunnel, "server_actual_state") << '\n'
              << "  Pending reason " << nullable_text(*tunnel, "pending_reason") << '\n'
              << "  Last sync (ms) "
              << (last_synced_at.has_value() ? std::to_string(*last_synced_at) : "-") << '\n';
    return true;
}

[[nodiscard]] bool print_removed(const minitun::ipc::Json& result,
                                 const std::string_view resource) {
    const auto removed = result.find("removed");
    if (removed == result.end() || !removed->is_object()) {
        return false;
    }
    const auto id = removed->find("id");
    const auto name = removed->find("name");
    if (id == removed->end() || !id->is_string() || name == removed->end() ||
        !nullable_string(*name)) {
        return false;
    }
    std::cout << resource << " removed:\n"
              << "  ID    " << id->get<std::string>() << '\n'
              << "  Name  " << nullable_text(*removed, "name") << '\n';
    return true;
}

[[nodiscard]] bool print_daemon_status(const minitun::ipc::Json& result) {
    const auto state = result.find("state");
    const auto version = json_count(result, "ipc_version");
    if (result.size() != 2U || state == result.end() || !state->is_string() ||
        state->get_ref<const std::string&>() != "running" || !version.has_value() ||
        *version != minitun::ipc::kProtocolVersion) {
        return false;
    }
    std::cout << "Daemon status:\n"
              << "  State        running\n"
              << "  IPC version  " << minitun::ipc::kProtocolVersion << '\n';
    return true;
}

[[nodiscard]] bool print_daemon_identity(const minitun::ipc::Json& result, const bool json_output) {
    const auto client_id = result.find("client_id");
    if (result.size() != 1U || client_id == result.end() || !client_id->is_string() ||
        !minitun::common::Id::parse(client_id->get_ref<const std::string&>(),
                                    minitun::common::IdKind::client)) {
        return false;
    }
    if (json_output) {
        std::cout << result.dump(2) << '\n';
    } else {
        std::cout << "Daemon identity:\n  Client ID  " << client_id->get<std::string>() << '\n';
    }
    return true;
}

[[nodiscard]] bool print_status(const minitun::ipc::Json& result, const bool json_output) {
    if (json_output) {
        if (!result.is_object()) {
            return false;
        }
        std::cout << result.dump(2) << '\n';
        return true;
    }
    const auto daemon = result.find("daemon");
    const auto servers = result.find("servers");
    const auto tunnels = result.find("tunnels");
    if (daemon == result.end() || !daemon->is_object() || servers == result.end() ||
        !servers->is_object() || tunnels == result.end() || !tunnels->is_object()) {
        return false;
    }
    const auto daemon_state = daemon->find("state");
    const auto server_total = json_count(*servers, "total");
    const auto server_online = json_count(*servers, "online");
    const auto tunnel_total = json_count(*tunnels, "total");
    const auto tunnel_active = json_count(*tunnels, "active");
    if (daemon_state == daemon->end() || !daemon_state->is_string() ||
        daemon_state->get_ref<const std::string&>() != "running" || !server_total ||
        !server_online || !tunnel_total || !tunnel_active) {
        return false;
    }
    std::cout << "MiniTun status:\n"
              << "  Daemon    running\n"
              << "  Servers  " << *server_total << " total, " << *server_online << " online\n"
              << "  Tunnels  " << *tunnel_total << " total, " << *tunnel_active << " active\n";
    return true;
}

[[nodiscard]] bool print_json_object(const minitun::ipc::Json& result) {
    if (!result.is_object()) {
        return false;
    }
    std::cout << result.dump(2) << '\n';
    return true;
}

[[nodiscard]] minitun::common::Result<minitun::common::SecureString>
read_psk(const bool psk_stdin) {
    std::string psk;
    const StringScrubber psk_scrubber{psk};
    if (psk_stdin) {
        try {
            // Read at most one byte beyond the accepted limit. std::getline would
            // allocate for an arbitrarily large line before the length check below.
            for (;;) {
                const int value = std::cin.get();
                if (value == std::char_traits<char>::eof()) {
                    if (std::cin.eof() && !psk.empty()) {
                        break;
                    }
                    return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                                  "failed to read PSK from standard input"};
                }
                const char character = static_cast<char>(value);
                if (character == '\n' || character == '\r') {
                    break;
                }
                if (psk.size() == kMaxCredentialBytes) {
                    return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                                  "PSK is outside its accepted byte-length"};
                }
                psk.push_back(character);
            }
        } catch (const std::bad_alloc&) {
            return minitun::common::Error{minitun::common::ErrorCode::resource_exhausted,
                                          "insufficient memory while reading PSK"};
        } catch (const std::length_error&) {
            return minitun::common::Error{minitun::common::ErrorCode::resource_exhausted,
                                          "insufficient memory while reading PSK"};
        }
    } else {
        if (::isatty(STDIN_FILENO) == 0) {
            return minitun::common::Error{
                minitun::common::ErrorCode::invalid_argument,
                "interactive PSK input requires a terminal; use --psk-stdin for a pipe"};
        }
        TerminalInputGuard guard;
        auto started = guard.start();
        if (!started) {
            return started.error();
        }
        std::cerr << "PSK: " << std::flush;
        while (psk.size() <= kMaxCredentialBytes) {
            char character = '\0';
            const ssize_t read_count = ::read(STDIN_FILENO, &character, 1U);
            if (read_count == 1) {
                if (character == '\n' || character == '\r') {
                    break;
                }
                psk.push_back(character);
                continue;
            }
            if (read_count < 0 && errno == EINTR && terminal_signal != 0) {
                break;
            }
            minitun::common::secure_erase_memory(psk.data(), psk.size());
            return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                          "failed to read PSK from terminal"};
        }
        const int interrupted_signal = terminal_signal;
        guard.restore();
        std::cerr << '\n';
        if (interrupted_signal != 0) {
            terminal_signal = 0;
            static_cast<void>(::raise(interrupted_signal));
            minitun::common::secure_erase_memory(psk.data(), psk.size());
            return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                          "PSK input was interrupted"};
        }
    }

    if (psk.empty() || psk.size() > kMaxCredentialBytes || psk.find('\0') != std::string::npos) {
        minitun::common::secure_erase_memory(psk.data(), psk.size());
        return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                      "PSK is outside its accepted byte-length"};
    }
    try {
        minitun::common::SecureString secure_psk{psk};
        minitun::common::secure_erase_memory(psk.data(), psk.size());
        return secure_psk;
    } catch (...) {
        minitun::common::secure_erase_memory(psk.data(), psk.size());
        return minitun::common::Error{minitun::common::ErrorCode::resource_exhausted,
                                      "insufficient memory while reading PSK"};
    }
}

[[nodiscard]] minitun::common::Result<minitun::common::SecureString>
read_credential_file(const std::string& path, const bool require_private) {
    if (path.empty() || path.size() > 4'096U || path.find('\0') != std::string::npos) {
        return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                      "credential file path is invalid"};
    }
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return minitun::common::Error{minitun::common::ErrorCode::permission_denied,
                                      "credential file could not be opened"};
    }
    const auto close_descriptor = [&descriptor] { static_cast<void>(::close(descriptor)); };
    struct stat metadata{};
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_nlink != 1 || metadata.st_size <= 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > kMaxCredentialBytes) {
        close_descriptor();
        return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                      "credential file is not a bounded regular file"};
    }
    if (require_private &&
        (metadata.st_uid != ::geteuid() || (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0)) {
        close_descriptor();
        return minitun::common::Error{
            minitun::common::ErrorCode::permission_denied,
            "private credential file must be owned by the caller and mode 0600 or stricter"};
    }

    try {
        std::string contents(static_cast<std::size_t>(metadata.st_size), '\0');
        const StringScrubber scrubber{contents};
        std::size_t offset = 0U;
        while (offset < contents.size()) {
            const ssize_t count =
                ::read(descriptor, contents.data() + offset, contents.size() - offset);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            close_descriptor();
            return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                          "credential file could not be read completely"};
        }
        close_descriptor();
        if (contents.find('\0') != std::string::npos) {
            return minitun::common::Error{minitun::common::ErrorCode::invalid_argument,
                                          "credential file contains a NUL byte"};
        }
        return minitun::common::SecureString{contents};
    } catch (...) {
        close_descriptor();
        return minitun::common::Error{minitun::common::ErrorCode::resource_exhausted,
                                      "insufficient memory while reading credential file"};
    }
}

using Renderer = std::function<bool(const minitun::ipc::Json&)>;

int execute_request(const std::string& socket_path, std::string method, minitun::ipc::Json params,
                    const Renderer& renderer) {
    FailureOrigin origin = FailureOrigin::local_transport;
    auto result = request_daemon(socket_path, std::move(method), std::move(params), origin);
    if (!result) {
        return report_error(result.error(), origin);
    }
    if (!renderer(*result)) {
        std::cerr << "minitun: internal failure: daemon returned an invalid command result\n";
        return kInternalErrorExitCode;
    }
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
    CLI::App* const status_command = app.add_subcommand("status", "Show MiniTun status");
    bool status_json = false;
    status_command->add_flag("--json", status_json, "Print JSON");

    CLI::App* const doctor_command =
        app.add_subcommand("doctor", "Check, back up, or restore the local SQLite databases");
    bool doctor_json = false;
    bool doctor_checkpoint = false;
    std::string doctor_backup_state;
    std::string doctor_backup_credentials;
    std::string doctor_restore_state;
    std::string doctor_restore_credentials;
    doctor_command->add_flag("--json", doctor_json, "Print JSON");
    doctor_command->add_flag("--checkpoint", doctor_checkpoint,
                             "Checkpoint the state database before reporting");
    doctor_command->add_option("--backup-state", doctor_backup_state,
                               "Write an online state database backup");
    doctor_command->add_option("--backup-credentials", doctor_backup_credentials,
                               "Write an online credentials database backup");
    doctor_command->add_option("--restore-state", doctor_restore_state,
                               "Restore the state database from a private backup");
    doctor_command->add_option("--restore-credentials", doctor_restore_credentials,
                               "Restore the credentials database from a private backup");

    CLI::App* const health_command = app.add_subcommand("health", "Check local daemon health");
    CLI::App* const readiness_command =
        app.add_subcommand("readiness", "Check whether the daemon is ready");
    CLI::App* const metrics_command = app.add_subcommand("metrics", "Print local metrics");
    CLI::App* const reload_command = app.add_subcommand("reload", "Reload daemon configuration");

    CLI::App* const config_command =
        app.add_subcommand("config", "Export, plan, or apply declarative configuration");
    CLI::App* const config_export_command =
        config_command->add_subcommand("export", "Export configuration as strict JSON");
    CLI::App* const config_plan_command =
        config_command->add_subcommand("plan", "Plan declarative configuration changes");
    std::string config_plan_path;
    bool config_plan_prune = false;
    config_plan_command->add_option("file", config_plan_path, "format_version 1 JSON file")
        ->required();
    config_plan_command->add_flag("--prune", config_plan_prune,
                                  "Include deletion of missing apply-managed resources");
    CLI::App* const config_apply_command =
        config_command->add_subcommand("apply", "Apply declarative configuration changes");
    std::string config_apply_path;
    bool config_apply_prune = false;
    config_apply_command->add_option("file", config_apply_path, "format_version 1 JSON file")
        ->required();
    config_apply_command->add_flag("--prune", config_apply_prune,
                                   "Delete missing apply-managed resources");
    config_command->require_subcommand(1);

    CLI::App* const daemon_command = app.add_subcommand("daemon", "Inspect the local daemon");
    CLI::App* const daemon_status_command =
        daemon_command->add_subcommand("status", "Show local daemon status");
    CLI::App* const daemon_identity_command =
        daemon_command->add_subcommand("identity", "Show the stable remote client identity");
    bool daemon_identity_json = false;
    daemon_identity_command->add_flag("--json", daemon_identity_json, "Print JSON");
    daemon_command->require_subcommand(1);

    CLI::App* const server_command = app.add_subcommand("server", "Manage public servers");
    CLI::App* const server_add_command =
        server_command->add_subcommand("add", "Add a public server");
    std::string server_add_endpoint;
    std::string server_add_name;
    server_add_command->add_option("server-endpoint", server_add_endpoint, "Server host and port")
        ->required();
    CLI::Option* const server_add_name_option =
        server_add_command->add_option("--name", server_add_name, "Server name");

    CLI::App* const server_login_command =
        server_command->add_subcommand("login", "Store a server pre-shared key (PSK)");
    std::string server_login_identifier;
    bool psk_stdin = false;
    server_login_command
        ->add_option("server-id-or-name", server_login_identifier, "Server ID or name")
        ->required();
    server_login_command->add_flag("--psk-stdin,--token-stdin", psk_stdin,
                                   "Read one PSK line from standard input");

    CLI::App* const server_update_command =
        server_command->add_subcommand("update", "Update one public server");
    std::string server_update_identifier;
    std::string server_update_name;
    std::string server_update_endpoint;
    std::string server_update_tls_server_name;
    std::string server_update_ca_file;
    std::string server_update_client_certificate_file;
    std::string server_update_client_private_key_file;
    bool server_update_clear_name = false;
    bool server_update_clear_tls_server_name = false;
    bool server_update_clear_ca = false;
    bool server_update_clear_client_identity = false;
    server_update_command
        ->add_option("server-id-or-name", server_update_identifier, "Server ID or name")
        ->required();
    CLI::Option* const server_update_name_option =
        server_update_command->add_option("--name", server_update_name, "New server name");
    CLI::Option* const server_update_endpoint_option = server_update_command->add_option(
        "--endpoint", server_update_endpoint, "New server host and port");
    CLI::Option* const server_update_tls_server_name_option = server_update_command->add_option(
        "--tls-server-name", server_update_tls_server_name, "New TLS SNI and verification name");
    CLI::Option* const server_update_ca_file_option = server_update_command->add_option(
        "--ca-file", server_update_ca_file, "PEM CA certificate file to store locally");
    CLI::Option* const server_update_client_certificate_file_option =
        server_update_command->add_option("--client-cert", server_update_client_certificate_file,
                                          "PEM client certificate chain file to store locally");
    CLI::Option* const server_update_client_private_key_file_option =
        server_update_command->add_option("--client-key", server_update_client_private_key_file,
                                          "Private PEM client key file to store locally");
    server_update_command->add_flag("--clear-name", server_update_clear_name,
                                    "Remove the optional server name");
    server_update_command->add_flag("--clear-tls-server-name", server_update_clear_tls_server_name,
                                    "Use the endpoint host for TLS verification");
    server_update_command->add_flag("--clear-ca", server_update_clear_ca,
                                    "Use the daemon's default trust store");
    server_update_command->add_flag("--clear-client-identity", server_update_clear_client_identity,
                                    "Delete the client certificate and private key");

    CLI::App* const server_enable_command =
        server_command->add_subcommand("enable", "Enable one public server");
    std::string server_enable_identifier;
    server_enable_command
        ->add_option("server-id-or-name", server_enable_identifier, "Server ID or name")
        ->required();
    CLI::App* const server_disable_command =
        server_command->add_subcommand("disable", "Disable one public server without removing it");
    std::string server_disable_identifier;
    server_disable_command
        ->add_option("server-id-or-name", server_disable_identifier, "Server ID or name")
        ->required();
    CLI::App* const server_logout_command =
        server_command->add_subcommand("logout", "Delete authentication material for one server");
    std::string server_logout_identifier;
    server_logout_command
        ->add_option("server-id-or-name", server_logout_identifier, "Server ID or name")
        ->required();

    CLI::App* const server_list_command =
        server_command->add_subcommand("list", "List configured servers");
    bool server_list_json = false;
    server_list_command->add_flag("--json", server_list_json, "Print JSON");

    CLI::App* const server_inspect_command =
        server_command->add_subcommand("inspect", "Inspect one server");
    std::string server_inspect_identifier;
    bool server_inspect_json = false;
    server_inspect_command
        ->add_option("server-id-or-name", server_inspect_identifier, "Server ID or name")
        ->required();
    server_inspect_command->add_flag("--json", server_inspect_json, "Print JSON");

    CLI::App* const server_remove_command =
        server_command->add_subcommand("remove", "Remove one server");
    std::string server_remove_identifier;
    server_remove_command
        ->add_option("server-id-or-name", server_remove_identifier, "Server ID or name")
        ->required();
    server_command->require_subcommand(1);

    CLI::App* const tunnel_command = app.add_subcommand("tun", "Manage TCP tunnels");
    CLI::App* const tunnel_add_command = tunnel_command->add_subcommand("add", "Add a TCP tunnel");
    std::string tunnel_add_server;
    int tunnel_add_local_port = 0;
    int tunnel_add_remote_port = 0;
    std::string tunnel_add_local_host{"127.0.0.1"};
    std::string tunnel_add_name;
    tunnel_add_command->add_option("server-id-or-name", tunnel_add_server, "Server ID or name")
        ->required();
    tunnel_add_command->add_option("local-port", tunnel_add_local_port, "Local TCP port")
        ->required()
        ->check(CLI::Range(1, 65'535));
    tunnel_add_command->add_option("server-port", tunnel_add_remote_port, "Public server TCP port")
        ->required()
        ->check(CLI::Range(1, 65'535));
    tunnel_add_command->add_option("--local-host", tunnel_add_local_host, "Local target host")
        ->capture_default_str();
    CLI::Option* const tunnel_add_name_option =
        tunnel_add_command->add_option("--name", tunnel_add_name, "Tunnel name");

    CLI::App* const tunnel_update_command =
        tunnel_command->add_subcommand("update", "Update one TCP tunnel");
    std::string tunnel_update_identifier;
    std::string tunnel_update_name;
    std::string tunnel_update_local_host;
    int tunnel_update_local_port = 0;
    int tunnel_update_remote_port = 0;
    bool tunnel_update_clear_name = false;
    tunnel_update_command
        ->add_option("tun-id-or-name", tunnel_update_identifier, "Tunnel ID or name")
        ->required();
    CLI::Option* const tunnel_update_name_option =
        tunnel_update_command->add_option("--name", tunnel_update_name, "New tunnel name");
    CLI::Option* const tunnel_update_local_host_option = tunnel_update_command->add_option(
        "--local-host", tunnel_update_local_host, "New local target host");
    CLI::Option* const tunnel_update_local_port_option =
        tunnel_update_command
            ->add_option("--local-port", tunnel_update_local_port, "New local target port")
            ->check(CLI::Range(1, 65'535));
    CLI::Option* const tunnel_update_remote_port_option =
        tunnel_update_command
            ->add_option("--server-port", tunnel_update_remote_port, "New public server port")
            ->check(CLI::Range(1, 65'535));
    tunnel_update_command->add_flag("--clear-name", tunnel_update_clear_name,
                                    "Remove the optional tunnel name");

    CLI::App* const tunnel_enable_command =
        tunnel_command->add_subcommand("enable", "Enable one TCP tunnel");
    std::string tunnel_enable_identifier;
    tunnel_enable_command
        ->add_option("tun-id-or-name", tunnel_enable_identifier, "Tunnel ID or name")
        ->required();
    CLI::App* const tunnel_disable_command =
        tunnel_command->add_subcommand("disable", "Disable one TCP tunnel without removing it");
    std::string tunnel_disable_identifier;
    tunnel_disable_command
        ->add_option("tun-id-or-name", tunnel_disable_identifier, "Tunnel ID or name")
        ->required();

    CLI::App* const tunnel_list_command =
        tunnel_command->add_subcommand("list", "List configured tunnels");
    std::string tunnel_list_server;
    bool tunnel_list_json = false;
    CLI::Option* const tunnel_list_server_option = tunnel_list_command->add_option(
        "server-id-or-name", tunnel_list_server, "Optional server ID or name");
    tunnel_list_command->add_flag("--json", tunnel_list_json, "Print JSON");

    CLI::App* const tunnel_inspect_command =
        tunnel_command->add_subcommand("inspect", "Inspect one tunnel");
    std::string tunnel_inspect_identifier;
    bool tunnel_inspect_json = false;
    tunnel_inspect_command
        ->add_option("tun-id-or-name", tunnel_inspect_identifier, "Tunnel ID or name")
        ->required();
    tunnel_inspect_command->add_flag("--json", tunnel_inspect_json, "Print JSON");

    CLI::App* const tunnel_remove_command =
        tunnel_command->add_subcommand("remove", "Remove one tunnel");
    std::string tunnel_remove_identifier;
    tunnel_remove_command
        ->add_option("tun-id-or-name", tunnel_remove_identifier, "Tunnel ID or name")
        ->required();
    tunnel_command->require_subcommand(1);
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
        return kSuccessExitCode;
    }

    try {
        if (daemon_status_command->parsed()) {
            return execute_request(socket_path, "daemon.status", minitun::ipc::Json::object(),
                                   print_daemon_status);
        }
        if (daemon_identity_command->parsed()) {
            return execute_request(socket_path, "daemon.identity", minitun::ipc::Json::object(),
                                   [daemon_identity_json](const auto& result) {
                                       return print_daemon_identity(result, daemon_identity_json);
                                   });
        }
        if (status_command->parsed()) {
            return execute_request(
                socket_path, "status", minitun::ipc::Json::object(),
                [status_json](const auto& result) { return print_status(result, status_json); });
        }
        if (doctor_command->parsed()) {
            minitun::ipc::Json params = minitun::ipc::Json::object();
            if (!doctor_backup_state.empty()) {
                params["backup_state"] = doctor_backup_state;
            }
            if (!doctor_backup_credentials.empty()) {
                params["backup_credentials"] = doctor_backup_credentials;
            }
            if (!doctor_restore_state.empty()) {
                params["restore_state"] = doctor_restore_state;
            }
            if (!doctor_restore_credentials.empty()) {
                params["restore_credentials"] = doctor_restore_credentials;
            }
            if (doctor_checkpoint) {
                params["checkpoint"] = true;
            }
            return execute_request(
                socket_path, "doctor", std::move(params), [doctor_json](const auto& result) {
                    if (doctor_json) {
                        return print_json_object(result);
                    }
                    const auto ok = result.find("ok");
                    if (ok == result.end() || !ok->is_boolean()) {
                        return false;
                    }
                    std::cout << "MiniTun doctor: "
                              << (ok->template get<bool>() ? "healthy" : "degraded") << '\n';
                    return true;
                });
        }
        if (health_command->parsed()) {
            return execute_request(socket_path, "health", minitun::ipc::Json::object(),
                                   print_json_object);
        }
        if (readiness_command->parsed()) {
            return execute_request(socket_path, "readiness", minitun::ipc::Json::object(),
                                   print_json_object);
        }
        if (metrics_command->parsed()) {
            return execute_request(socket_path, "metrics", minitun::ipc::Json::object(),
                                   print_json_object);
        }
        if (reload_command->parsed()) {
            return execute_request(socket_path, "reload", minitun::ipc::Json::object(),
                                   print_json_object);
        }
        if (config_export_command->parsed()) {
            return execute_request(socket_path, "config.export", minitun::ipc::Json::object(),
                                   print_json_object);
        }
        if (config_plan_command->parsed()) {
            return execute_request(
                socket_path, "config.plan",
                minitun::ipc::Json{{"path", config_plan_path}, {"prune", config_plan_prune}},
                print_json_object);
        }
        if (config_apply_command->parsed()) {
            return execute_request(
                socket_path, "config.apply",
                minitun::ipc::Json{{"path", config_apply_path}, {"prune", config_apply_prune}},
                print_json_object);
        }
        if (server_add_command->parsed()) {
            minitun::ipc::Json params{{"endpoint", server_add_endpoint}};
            if (server_add_name_option->count() != 0U) {
                params["name"] = server_add_name;
            }
            return execute_request(
                socket_path, "server.add", std::move(params),
                [](const auto& result) { return print_server_summary(result, "Server added"); });
        }
        if (server_login_command->parsed()) {
            auto psk = read_psk(psk_stdin);
            if (!psk) {
                return report_error(psk.error(), FailureOrigin::daemon);
            }
            minitun::ipc::Json params{
                {"identifier", server_login_identifier},
                {"psk", std::string{psk->view()}},
            };
            psk->clear();
            return execute_request(
                socket_path, "server.login", std::move(params), [](const auto& result) {
                    return print_server_summary(result, "Server credentials stored");
                });
        }
        if (server_update_command->parsed()) {
            minitun::ipc::Json params{{"identifier", server_update_identifier}};
            if (server_update_name_option->count() != 0U) {
                params["name"] = server_update_name;
            } else if (server_update_clear_name) {
                params["name"] = nullptr;
            }
            if (server_update_endpoint_option->count() != 0U) {
                params["endpoint"] = server_update_endpoint;
            }
            if (server_update_tls_server_name_option->count() != 0U) {
                params["tls_server_name"] = server_update_tls_server_name;
            } else if (server_update_clear_tls_server_name) {
                params["tls_server_name"] = nullptr;
            }
            const auto add_credential_file = [&params](const std::string_view field,
                                                       const std::string& path,
                                                       const bool require_private) -> int {
                auto material = read_credential_file(path, require_private);
                if (!material) {
                    return report_error(material.error(), FailureOrigin::daemon);
                }
                params[std::string{field}] = std::string{material->view()};
                material->clear();
                return kSuccessExitCode;
            };
            if (server_update_ca_file_option->count() != 0U) {
                const int result =
                    add_credential_file("ca_certificate", server_update_ca_file, false);
                if (result != kSuccessExitCode) {
                    return result;
                }
            } else if (server_update_clear_ca) {
                params["ca_certificate"] = nullptr;
            }
            if (server_update_client_certificate_file_option->count() != 0U) {
                const int result = add_credential_file(
                    "client_certificate", server_update_client_certificate_file, false);
                if (result != kSuccessExitCode) {
                    return result;
                }
            } else if (server_update_clear_client_identity) {
                params["client_certificate"] = nullptr;
            }
            if (server_update_client_private_key_file_option->count() != 0U) {
                const int result = add_credential_file("client_private_key",
                                                       server_update_client_private_key_file, true);
                if (result != kSuccessExitCode) {
                    return result;
                }
            } else if (server_update_clear_client_identity) {
                params["client_private_key"] = nullptr;
            }
            return execute_request(
                socket_path, "server.update", std::move(params),
                [](const auto& result) { return print_server_summary(result, "Server updated"); });
        }
        if (server_enable_command->parsed()) {
            return execute_request(
                socket_path, "server.enable",
                minitun::ipc::Json{{"identifier", server_enable_identifier}},
                [](const auto& result) { return print_server_summary(result, "Server enabled"); });
        }
        if (server_disable_command->parsed()) {
            return execute_request(
                socket_path, "server.disable",
                minitun::ipc::Json{{"identifier", server_disable_identifier}},
                [](const auto& result) { return print_server_summary(result, "Server disabled"); });
        }
        if (server_logout_command->parsed()) {
            return execute_request(socket_path, "server.logout",
                                   minitun::ipc::Json{{"identifier", server_logout_identifier}},
                                   [](const auto& result) {
                                       return print_server_summary(result, "Server logged out");
                                   });
        }
        if (server_list_command->parsed()) {
            return execute_request(socket_path, "server.list", minitun::ipc::Json::object(),
                                   [server_list_json](const auto& result) {
                                       return print_server_list(result, server_list_json);
                                   });
        }
        if (server_inspect_command->parsed()) {
            return execute_request(socket_path, "server.inspect",
                                   minitun::ipc::Json{{"identifier", server_inspect_identifier}},
                                   [server_inspect_json](const auto& result) {
                                       return print_server_inspect(result, server_inspect_json);
                                   });
        }
        if (server_remove_command->parsed()) {
            return execute_request(
                socket_path, "server.remove",
                minitun::ipc::Json{{"identifier", server_remove_identifier}},
                [](const auto& result) { return print_removed(result, "Server"); });
        }
        if (tunnel_add_command->parsed()) {
            minitun::ipc::Json params{
                {"server", tunnel_add_server},
                {"local_port", tunnel_add_local_port},
                {"remote_port", tunnel_add_remote_port},
                {"local_host", tunnel_add_local_host},
            };
            if (tunnel_add_name_option->count() != 0U) {
                params["name"] = tunnel_add_name;
            }
            return execute_request(
                socket_path, "tun.add", std::move(params),
                [](const auto& result) { return print_tunnel_summary(result, "Tunnel added"); });
        }
        if (tunnel_update_command->parsed()) {
            minitun::ipc::Json params{{"identifier", tunnel_update_identifier}};
            if (tunnel_update_name_option->count() != 0U) {
                params["name"] = tunnel_update_name;
            } else if (tunnel_update_clear_name) {
                params["name"] = nullptr;
            }
            if (tunnel_update_local_host_option->count() != 0U) {
                params["local_host"] = tunnel_update_local_host;
            }
            if (tunnel_update_local_port_option->count() != 0U) {
                params["local_port"] = tunnel_update_local_port;
            }
            if (tunnel_update_remote_port_option->count() != 0U) {
                params["remote_port"] = tunnel_update_remote_port;
            }
            return execute_request(
                socket_path, "tun.update", std::move(params),
                [](const auto& result) { return print_tunnel_summary(result, "Tunnel updated"); });
        }
        if (tunnel_enable_command->parsed()) {
            return execute_request(
                socket_path, "tun.enable",
                minitun::ipc::Json{{"identifier", tunnel_enable_identifier}},
                [](const auto& result) { return print_tunnel_summary(result, "Tunnel enabled"); });
        }
        if (tunnel_disable_command->parsed()) {
            return execute_request(
                socket_path, "tun.disable",
                minitun::ipc::Json{{"identifier", tunnel_disable_identifier}},
                [](const auto& result) { return print_tunnel_summary(result, "Tunnel disabled"); });
        }
        if (tunnel_list_command->parsed()) {
            minitun::ipc::Json params = minitun::ipc::Json::object();
            if (tunnel_list_server_option->count() != 0U) {
                params["server"] = tunnel_list_server;
            }
            return execute_request(socket_path, "tun.list", std::move(params),
                                   [tunnel_list_json](const auto& result) {
                                       return print_tunnel_list(result, tunnel_list_json);
                                   });
        }
        if (tunnel_inspect_command->parsed()) {
            return execute_request(socket_path, "tun.inspect",
                                   minitun::ipc::Json{{"identifier", tunnel_inspect_identifier}},
                                   [tunnel_inspect_json](const auto& result) {
                                       return print_tunnel_inspect(result, tunnel_inspect_json);
                                   });
        }
        if (tunnel_remove_command->parsed()) {
            return execute_request(
                socket_path, "tun.remove",
                minitun::ipc::Json{{"identifier", tunnel_remove_identifier}},
                [](const auto& result) { return print_removed(result, "Tunnel"); });
        }
    } catch (...) {
        std::cerr << "minitun: internal failure: unexpected command processing failure\n";
        return kInternalErrorExitCode;
    }

    return kSuccessExitCode;
}
