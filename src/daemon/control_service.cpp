#include <minitun/daemon/control_service.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/daemon/credential_keys.hpp>
#include <minitun/daemon/declarative_config.hpp>
#include <minitun/ipc/dispatcher.hpp>
#include <minitun/protocol/tls.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/models.hpp>
#include <minitun/storage/state_repository.hpp>

namespace minitun::daemon {
namespace {

using common::Error;
using common::ErrorCode;
using common::Result;
using ipc::Json;
using storage::ServerActualState;
using storage::ServerDesiredState;
using storage::ServerRecord;
using storage::TunnelActualState;
using storage::TunnelDesiredState;
using storage::TunnelRecord;

[[nodiscard]] Result<void> validate_protocol_binding(const storage::TunnelProtocol protocol,
                                                     const common::Endpoint& remote_endpoint) {
    if (protocol != storage::TunnelProtocol::socks5) {
        return Result<void>::success();
    }
    asio::error_code error;
    const auto address = asio::ip::make_address(remote_endpoint.host(), error);
    if (error || !address.is_loopback()) {
        return Error{ErrorCode::permission_denied, "SOCKS5 tunnels must bind a loopback address"};
    }
    return Result<void>::success();
}

class StringScrubber final {
  public:
    explicit StringScrubber(std::string& value) noexcept : value_(value) {}
    ~StringScrubber() noexcept {
        common::secure_erase_memory(value_.data(), value_.size());
        value_.clear();
    }

    StringScrubber(const StringScrubber&) = delete;
    StringScrubber& operator=(const StringScrubber&) = delete;

  private:
    std::string& value_;
};

class StagedCredentials final {
  public:
    explicit StagedCredentials(storage::CredentialStore& store) noexcept : store_(store) {}

    [[nodiscard]] Result<void> put(const std::string& key, const std::string_view value) {
        try {
            keys_.push_back(key);
        } catch (...) {
            return Error{ErrorCode::resource_exhausted,
                         "insufficient memory while staging server credentials"};
        }
        auto stored = store_.put(key, value);
        if (!stored) {
            keys_.pop_back();
            return stored.error();
        }
        return Result<void>::success();
    }

    void release() noexcept { cleanup_ = false; }

    ~StagedCredentials() noexcept {
        if (!cleanup_) {
            return;
        }
        for (const auto& key : keys_) {
            static_cast<void>(store_.remove(key));
        }
    }

    StagedCredentials(const StagedCredentials&) = delete;
    StagedCredentials& operator=(const StagedCredentials&) = delete;

  private:
    storage::CredentialStore& store_;
    std::vector<std::string> keys_;
    bool cleanup_{true};
};

[[nodiscard]] Result<common::SecureString>
load_credential(storage::CredentialStore& credentials, const std::optional<std::string>& reference,
                const std::string_view description) {
    if (!reference.has_value()) {
        return common::SecureString{};
    }
    auto loaded = credentials.get(*reference);
    if (!loaded) {
        return Error{loaded.error().code(), std::string{description} + " is unavailable"};
    }
    return loaded;
}

[[nodiscard]] Result<common::SecureString>
parse_credential_material(const std::optional<std::optional<std::string>>& field,
                          common::SecureString current, const std::string_view description) {
    if (!field.has_value()) {
        return current;
    }
    if (!field->has_value()) {
        return common::SecureString{};
    }
    const std::string_view value = **field;
    if (value.empty() || value.size() > storage::kMaxCredentialSecretBytes ||
        value.find('\0') != std::string_view::npos) {
        return Error{ErrorCode::invalid_argument,
                     std::string{description} + " is outside its accepted byte-length"};
    }
    try {
        return common::SecureString{value};
    } catch (...) {
        return Error{ErrorCode::resource_exhausted,
                     "insufficient memory while processing server credentials"};
    }
}

[[nodiscard]] bool contains(const std::initializer_list<std::string_view> fields,
                            const std::string_view value) {
    return std::find(fields.begin(), fields.end(), value) != fields.end();
}

[[nodiscard]] Result<void>
validate_params(const Json& params, const std::initializer_list<std::string_view> required,
                const std::initializer_list<std::string_view> optional = {}) {
    for (const auto& field : required) {
        if (!params.contains(field)) {
            return Result<void>::failure(ErrorCode::invalid_argument,
                                         "IPC params are missing a required field");
        }
    }
    for (auto item = params.cbegin(); item != params.cend(); ++item) {
        if (!contains(required, item.key()) && !contains(optional, item.key())) {
            return Result<void>::failure(ErrorCode::invalid_argument,
                                         "IPC params contain an unknown field");
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::string> required_string(const Json& params,
                                                  const std::string_view field) {
    const auto value = params.find(field);
    if (value == params.end() || !value->is_string()) {
        return Result<std::string>::failure(ErrorCode::invalid_argument,
                                            "IPC parameter must be a string");
    }
    return value->get<std::string>();
}

[[nodiscard]] Result<std::optional<std::string>> optional_string(const Json& params,
                                                                 const std::string_view field) {
    const auto value = params.find(field);
    if (value == params.end()) {
        return std::optional<std::string>{};
    }
    if (!value->is_string()) {
        return Result<std::optional<std::string>>::failure(ErrorCode::invalid_argument,
                                                           "IPC parameter must be a string");
    }
    return std::optional<std::string>{value->get<std::string>()};
}

// The outer optional distinguishes an omitted field from an explicit null,
// which is used by update methods to clear nullable configuration.
[[nodiscard]] Result<std::optional<std::optional<std::string>>>
optional_nullable_string(const Json& params, const std::string_view field) {
    const auto value = params.find(field);
    if (value == params.end()) {
        return std::optional<std::optional<std::string>>{};
    }
    if (value->is_null()) {
        return std::optional<std::optional<std::string>>{std::in_place, std::nullopt};
    }
    if (!value->is_string()) {
        return Result<std::optional<std::optional<std::string>>>::failure(
            ErrorCode::invalid_argument, "IPC parameter must be a string or null");
    }
    return std::optional<std::optional<std::string>>{
        std::in_place, std::optional<std::string>{value->get<std::string>()}};
}

[[nodiscard]] Result<std::uint16_t> required_port(const Json& params,
                                                  const std::string_view field) {
    const auto value = params.find(field);
    if (value == params.end() || (!value->is_number_integer() && !value->is_number_unsigned())) {
        return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                              "port parameter must be an integer");
    }
    std::uint64_t port = 0;
    if (value->is_number_unsigned()) {
        port = value->get<std::uint64_t>();
    } else {
        const std::int64_t signed_port = value->get<std::int64_t>();
        if (signed_port < 0) {
            return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                                  "port must be between 1 and 65535");
        }
        port = static_cast<std::uint64_t>(signed_port);
    }
    if (port < 1U || port > 65'535U) {
        return Result<std::uint16_t>::failure(ErrorCode::invalid_argument,
                                              "port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(port);
}

[[nodiscard]] Result<std::optional<std::uint16_t>> optional_port(const Json& params,
                                                                 const std::string_view field) {
    if (!params.contains(field)) {
        return std::optional<std::uint16_t>{};
    }
    auto port = required_port(params, field);
    if (!port) {
        return port.error();
    }
    return std::optional<std::uint16_t>{*port};
}

[[nodiscard]] Result<bool> optional_bool(const Json& params, const std::string_view field,
                                         const bool default_value = false) {
    const auto value = params.find(field);
    if (value == params.end()) {
        return default_value;
    }
    if (!value->is_boolean()) {
        return Error{ErrorCode::invalid_argument, "IPC parameter must be a boolean"};
    }
    return value->get<bool>();
}

[[nodiscard]] std::string endpoint_text(const std::string_view host, const std::uint16_t port) {
    std::string text;
    if (host.find(':') != std::string_view::npos) {
        text.push_back('[');
        text.append(host);
        text.push_back(']');
    } else {
        text.append(host);
    }
    text.push_back(':');
    text.append(std::to_string(port));
    return text;
}

[[nodiscard]] Result<ServerRecord> resolve_server(storage::StateRepository& repository,
                                                  const std::string_view identifier) {
    auto parsed = common::Id::parse(identifier, common::IdKind::server);
    Result<ServerRecord> server = parsed ? repository.servers().get_by_id(*parsed)
                                         : repository.servers().get_by_name(identifier);
    if (!server) {
        return server.error();
    }
    if (server->desired_state == ServerDesiredState::removed) {
        return Error{ErrorCode::not_found, "server was not found"};
    }
    return server;
}

[[nodiscard]] Result<TunnelRecord> resolve_tunnel(storage::StateRepository& repository,
                                                  const std::string_view identifier) {
    auto parsed = common::Id::parse(identifier, common::IdKind::tunnel);
    if (parsed) {
        auto tunnel = repository.tunnels().get_by_id(*parsed);
        if (!tunnel) {
            return tunnel.error();
        }
        if (tunnel->desired_state == TunnelDesiredState::removed) {
            return Error{ErrorCode::not_found, "tunnel was not found"};
        }
        return tunnel;
    }

    if (identifier.empty() || identifier.size() > storage::kMaxNameBytes) {
        return Error{ErrorCode::invalid_argument, "tunnel name is outside its byte-length limit"};
    }
    auto tunnels = repository.tunnels().list();
    if (!tunnels) {
        return tunnels.error();
    }
    std::optional<TunnelRecord> match;
    for (auto& tunnel : *tunnels) {
        if (tunnel.desired_state == TunnelDesiredState::removed || !tunnel.name.has_value() ||
            *tunnel.name != identifier) {
            continue;
        }
        if (match.has_value()) {
            return Error{ErrorCode::invalid_argument,
                         "tunnel name is ambiguous; use the tunnel ID"};
        }
        match = std::move(tunnel);
    }
    if (!match.has_value()) {
        return Error{ErrorCode::not_found, "tunnel was not found"};
    }
    return std::move(*match);
}

[[nodiscard]] Json optional_json(const std::optional<std::string>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

[[nodiscard]] Json optional_json(const std::optional<std::int64_t> value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

struct TunnelServerContext final {
    std::optional<std::string> name;
    ServerActualState actual_state;
};

[[nodiscard]] std::optional<std::string_view>
pending_reason(const TunnelRecord& tunnel, const TunnelServerContext* const server) noexcept {
    if (tunnel.actual_state != TunnelActualState::pending) {
        return std::nullopt;
    }
    if (server == nullptr) {
        return "server_not_found";
    }
    switch (server->actual_state) {
    case ServerActualState::not_authenticated:
        return "server_not_authenticated";
    case ServerActualState::disconnected:
        return "server_disconnected";
    case ServerActualState::connecting:
        return "server_connecting";
    case ServerActualState::tls_handshake:
        return "server_tls_handshake";
    case ServerActualState::authenticating:
        return "server_authenticating";
    case ServerActualState::online:
        return "awaiting_remote_sync";
    case ServerActualState::backoff:
        return "server_backoff";
    case ServerActualState::disabled:
        return "server_disabled";
    case ServerActualState::error:
        return "server_error";
    }
    return "server_state_unknown";
}

[[nodiscard]] Json last_error_json(const std::optional<ErrorCode> code,
                                   const std::optional<std::string>& message) {
    if (!code.has_value()) {
        return nullptr;
    }
    return Json{{"code", std::string{common::to_string(*code)}},
                {"message", optional_json(message)}};
}

[[nodiscard]] Json server_json(const ServerRecord& server, const std::size_t tunnel_count) {
    return Json{
        {"id", server.id.str()},
        {"name", optional_json(server.name)},
        {"endpoint", server.endpoint.to_string()},
        {"tls_server_name", optional_json(server.tls_server_name)},
        {"credential_configured", server.credential_ref.has_value()},
        {"ca_configured", server.ca_credential_ref.has_value()},
        {"client_certificate_configured",
         server.client_certificate_ref.has_value() && server.client_private_key_ref.has_value()},
        {"remote_server_id", optional_json(server.remote_server_id)},
        {"desired_state", std::string{storage::to_string(server.desired_state)}},
        {"actual_state", std::string{storage::to_string(server.actual_state)}},
        {"last_error", last_error_json(server.last_error_code, server.last_error_message)},
        {"reconnect_attempt", server.reconnect_attempt},
        {"latency_ms", optional_json(server.latency_ms)},
        {"tunnel_count", tunnel_count},
        {"config_revision", server.config_revision},
        {"managed_by_config", server.managed_by_config},
        {"created_at", server.created_at_unix_ms},
        {"updated_at", server.updated_at_unix_ms},
    };
}

[[nodiscard]] Json tunnel_json(const TunnelRecord& tunnel,
                               const TunnelServerContext* const server) {
    const auto reason = pending_reason(tunnel, server);
    return Json{
        {"id", tunnel.id.str()},
        {"name", optional_json(tunnel.name)},
        {"server_id", tunnel.server_id.str()},
        {"server_name", server == nullptr ? Json(nullptr) : optional_json(server->name)},
        {"server_actual_state", server == nullptr
                                    ? Json(nullptr)
                                    : Json(std::string{storage::to_string(server->actual_state)})},
        {"protocol", std::string{storage::to_string(tunnel.protocol)}},
        {"local_endpoint", tunnel.local_endpoint.to_string()},
        {"remote_endpoint", tunnel.remote_endpoint.to_string()},
        {"desired_state", std::string{storage::to_string(tunnel.desired_state)}},
        {"actual_state", std::string{storage::to_string(tunnel.actual_state)}},
        {"pending_reason", reason.has_value() ? Json(std::string{*reason}) : Json(nullptr)},
        {"last_error", last_error_json(tunnel.last_error_code, tunnel.last_error_message)},
        {"created_at", tunnel.created_at_unix_ms},
        {"updated_at", tunnel.updated_at_unix_ms},
        {"last_synced_at", optional_json(tunnel.last_synced_at_unix_ms)},
        {"config_revision", tunnel.config_revision},
        {"managed_by_config", tunnel.managed_by_config},
    };
}

[[nodiscard]] std::unordered_map<std::string, std::size_t>
tunnel_counts(const std::vector<TunnelRecord>& tunnels) {
    std::unordered_map<std::string, std::size_t> counts;
    for (const auto& tunnel : tunnels) {
        if (tunnel.desired_state != TunnelDesiredState::removed) {
            ++counts[tunnel.server_id.str()];
        }
    }
    return counts;
}

[[nodiscard]] std::int64_t update_time(const std::int64_t previous) noexcept {
    return std::max(previous, common::unix_milliseconds_now());
}

[[nodiscard]] Result<std::uint64_t> next_revision(const std::uint64_t current) {
    constexpr auto kMaximumRevision =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (current >= kMaximumRevision) {
        return Error{ErrorCode::resource_exhausted, "configuration revision is exhausted"};
    }
    return current + 1U;
}

[[nodiscard]] Json diagnostics_json(const storage::DatabaseDiagnostics& diagnostics) {
    return Json{
        {"path", diagnostics.path},
        {"file_size_bytes", diagnostics.file_size_bytes},
        {"file_mode", diagnostics.file_mode},
        {"owner_uid", diagnostics.owner_uid},
        {"device", diagnostics.device},
        {"inode", diagnostics.inode},
        {"schema_version", diagnostics.schema_version},
        {"journal_mode", diagnostics.journal_mode},
        {"synchronous", diagnostics.synchronous},
        {"foreign_keys", diagnostics.foreign_keys},
        {"schema_valid", diagnostics.schema_valid},
        {"integrity_ok", diagnostics.integrity_ok},
        {"integrity_result", diagnostics.integrity_result},
        {"page_count", diagnostics.page_count},
        {"freelist_count", diagnostics.freelist_count},
        {"wal_log_frames", diagnostics.wal_log_frames},
        {"wal_checkpointed_frames", diagnostics.wal_checkpointed_frames},
        {"wal_size_bytes", diagnostics.wal_size_bytes},
    };
}

} // namespace

ControlService::ControlService(storage::StateRepository& repository,
                               storage::CredentialStore& credentials,
                               std::function<void()> state_changed, JsonProvider runtime_metrics,
                               ReloadHandler reload_handler) noexcept
    : repository_(repository), credentials_(credentials), state_changed_(std::move(state_changed)),
      runtime_metrics_(std::move(runtime_metrics)), reload_handler_(std::move(reload_handler)) {}

void ControlService::notify_state_changed() const noexcept {
    if (!state_changed_) {
        return;
    }
    try {
        state_changed_();
    } catch (...) {
        common::log_error("failed to notify the remote session manager of committed state",
                          {.component = "daemon.control", .error_code = ErrorCode::internal_error});
    }
}

common::Result<void> ControlService::register_handlers(ipc::Dispatcher& dispatcher) {
    std::vector<std::string> registered;
    registered.reserve(27U);
    const auto add = [&dispatcher, &registered](std::string method,
                                                ipc::MethodHandler handler) -> Result<void> {
        std::string method_copy = method;
        constexpr std::array<std::string_view, 15U> audited_methods{
            "server.add",    "server.login",  "server.update", "server.enable", "server.disable",
            "server.logout", "server.remove", "tun.add",       "tun.update",    "tun.enable",
            "tun.disable",   "tun.remove",    "config.apply",  "doctor",        "reload",
        };
        const bool audited = std::find(audited_methods.begin(), audited_methods.end(), method) !=
                             audited_methods.end();
        if (audited) {
            handler = [method_name = method_copy, handler = std::move(handler)](
                          const ipc::Request& request) mutable -> Result<Json> {
                auto result = handler(request);
                const common::LogContext context{
                    .component = "daemon.audit",
                    .error_code =
                        result ? std::nullopt : std::optional<ErrorCode>{result.error().code()},
                };
                if (result) {
                    common::log_info("local management operation succeeded: " + method_name,
                                     context);
                } else {
                    common::log_warn("local management operation failed: " + method_name, context);
                }
                return result;
            };
        }
        auto result = dispatcher.register_handler(std::move(method), std::move(handler));
        if (result) {
            registered.push_back(std::move(method_copy));
        }
        return result;
    };

    const std::array registrations{
        std::pair{"daemon.status", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return daemon_status(request);
                  }}},
        std::pair{"daemon.identity", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return daemon_identity(request);
                  }}},
        std::pair{"server.add", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_add(request);
                  }}},
        std::pair{"server.login", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_login(request);
                  }}},
        std::pair{"server.update", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_update(request);
                  }}},
        std::pair{"server.enable", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_enable(request);
                  }}},
        std::pair{"server.disable", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_disable(request);
                  }}},
        std::pair{"server.logout", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_logout(request);
                  }}},
        std::pair{"server.list", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_list(request);
                  }}},
        std::pair{"server.inspect", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_inspect(request);
                  }}},
        std::pair{"server.remove", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_remove(request);
                  }}},
        std::pair{"tun.add", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_add(request);
                  }}},
        std::pair{"tun.update", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_update(request);
                  }}},
        std::pair{"tun.enable", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_enable(request);
                  }}},
        std::pair{"tun.disable", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_disable(request);
                  }}},
        std::pair{"tun.list", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_list(request);
                  }}},
        std::pair{"tun.inspect", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_inspect(request);
                  }}},
        std::pair{"tun.remove", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_remove(request);
                  }}},
        std::pair{"config.export", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return config_export(request);
                  }}},
        std::pair{"config.plan", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return config_plan(request);
                  }}},
        std::pair{"config.apply", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return config_apply(request);
                  }}},
        std::pair{"status", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return status(request);
                  }}},
        std::pair{"doctor", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return doctor(request);
                  }}},
        std::pair{"health", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return health(request);
                  }}},
        std::pair{"readiness", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return readiness(request);
                  }}},
        std::pair{"metrics", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return metrics(request);
                  }}},
        std::pair{"reload", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return reload(request);
                  }}},
    };

    for (const auto& [method, handler] : registrations) {
        auto result = add(method, handler);
        if (!result) {
            for (auto iterator = registered.rbegin(); iterator != registered.rend(); ++iterator) {
                static_cast<void>(dispatcher.unregister_handler(*iterator));
            }
            return result;
        }
    }
    return Result<void>::success();
}

Result<Json> ControlService::daemon_status(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    return Json{{"state", "running"}, {"ipc_version", ipc::kProtocolVersion}};
}

Result<Json> ControlService::daemon_identity(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    auto identity = repository_.client_id();
    if (!identity) {
        return identity.error();
    }
    return Json{{"client_id", identity->str()}};
}

Result<Json> ControlService::server_add(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"endpoint"}, {"name"}); !valid) {
        return valid.error();
    }
    auto endpoint_text_value = required_string(request.params, "endpoint");
    auto name = optional_string(request.params, "name");
    if (!endpoint_text_value) {
        return endpoint_text_value.error();
    }
    if (!name) {
        return name.error();
    }
    auto endpoint = common::Endpoint::parse(*endpoint_text_value);
    if (!endpoint) {
        return endpoint.error();
    }
    auto id = common::Id::generate(common::IdKind::server);
    if (!id) {
        return id.error();
    }
    const std::int64_t now = common::unix_milliseconds_now();
    ServerRecord server{
        .id = std::move(*id),
        .name = std::move(*name),
        .endpoint = std::move(*endpoint),
        .credential_ref = std::nullopt,
        .remote_server_id = std::nullopt,
        .desired_state = ServerDesiredState::enabled,
        .actual_state = ServerActualState::not_authenticated,
        .last_error_code = std::nullopt,
        .last_error_message = std::nullopt,
        .reconnect_attempt = 0,
        .latency_ms = std::nullopt,
        .created_at_unix_ms = now,
        .updated_at_unix_ms = now,
        .tls_server_name = std::nullopt,
        .ca_credential_ref = std::nullopt,
        .client_certificate_ref = std::nullopt,
        .client_private_key_ref = std::nullopt,
        .config_revision = 1U,
        .managed_by_config = false,
    };
    auto created = repository_.servers().create(server);
    if (!created) {
        return created.error();
    }
    notify_state_changed();
    return Json{{"server", server_json(server, 0U)}};
}

Result<Json> ControlService::server_login(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"}, {"psk", "token"}); !valid) {
        return valid.error();
    }
    const bool has_psk = request.params.contains("psk");
    const bool has_legacy_token = request.params.contains("token");
    if (has_psk == has_legacy_token) {
        return Error{ErrorCode::invalid_argument, "server login requires exactly one PSK field"};
    }
    auto identifier = required_string(request.params, "identifier");
    auto psk_text = required_string(request.params, has_psk ? "psk" : "token");
    if (!identifier) {
        return identifier.error();
    }
    if (!psk_text) {
        return psk_text.error();
    }
    const StringScrubber psk_scrubber{*psk_text};
    if (psk_text->empty() || psk_text->size() > storage::kMaxCredentialSecretBytes ||
        psk_text->find('\0') != std::string::npos) {
        return Error{ErrorCode::invalid_argument, "PSK is outside its accepted byte-length"};
    }
    common::SecureString psk{*psk_text};
    std::unique_lock credential_operation_lock{credential_operation_mutex_};

    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto server = resolve_server(repository_, *identifier);
    if (!server) {
        return server.error();
    }
    const std::optional<std::string> previous_key = server->credential_ref;
    const std::string key = next_credential_key(*server);
    auto revision = next_revision(server->config_revision);
    if (!revision) {
        return revision.error();
    }
    server->credential_ref = key;
    server->config_revision = *revision;
    server->actual_state = server->desired_state == ServerDesiredState::enabled
                               ? ServerActualState::disconnected
                               : ServerActualState::disabled;
    server->last_error_code.reset();
    server->last_error_message.reset();
    server->reconnect_attempt = 0;
    server->latency_ms.reset();
    server->updated_at_unix_ms = update_time(server->updated_at_unix_ms);

    auto updated = repository_.servers().update(*server, *transaction);
    if (!updated) {
        return updated.error();
    }
    auto tunnels = repository_.tunnels().list_by_server(server->id);
    if (!tunnels) {
        return tunnels.error();
    }
    const std::size_t count = tunnel_counts(*tunnels)[server->id.str()];
    auto stored = credentials_.put(key, psk.view());
    if (!stored) {
        return stored.error();
    }
    auto committed = transaction->commit();
    if (!committed) {
        auto cleaned = credentials_.remove(key);
        if (!cleaned) {
            common::log_error("failed to clean an uncommitted server credential",
                              {.component = "daemon.control",
                               .server_id = server->id.str(),
                               .error_code = cleaned.error().code()});
        }
        return committed.error();
    }
    auto cleaned = cleanup_server_credentials(
        credentials_, server->id,
        previous_key.has_value() ? std::optional<std::string_view>{*previous_key} : std::nullopt,
        std::string_view{key});
    if (!cleaned) {
        // The committed state references the new credential. Retaining an old
        // slot is safe and the next login or startup recovery will retry it.
        common::log_warn("deferred cleanup of previous or orphaned server credentials",
                         {.component = "daemon.control",
                          .server_id = server->id.str(),
                          .error_code = cleaned.error().code()});
    }
    credential_operation_lock.unlock();
    notify_state_changed();
    return Json{{"server", server_json(*server, count)}};
}

Result<Json> ControlService::server_update(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"},
                                     {"name", "endpoint", "tls_server_name", "ca_certificate",
                                      "client_certificate", "client_private_key"});
        !valid) {
        return valid.error();
    }
    if (request.params.size() == 1U) {
        return Error{ErrorCode::invalid_argument, "server update requires at least one field"};
    }
    auto identifier = required_string(request.params, "identifier");
    auto name = optional_nullable_string(request.params, "name");
    auto endpoint_text_value = optional_string(request.params, "endpoint");
    auto tls_server_name = optional_nullable_string(request.params, "tls_server_name");
    auto ca_certificate = optional_nullable_string(request.params, "ca_certificate");
    auto client_certificate = optional_nullable_string(request.params, "client_certificate");
    auto client_private_key = optional_nullable_string(request.params, "client_private_key");
    if (!identifier || !name || !endpoint_text_value || !tls_server_name || !ca_certificate ||
        !client_certificate || !client_private_key) {
        return !identifier            ? identifier.error()
               : !name                ? name.error()
               : !endpoint_text_value ? endpoint_text_value.error()
               : !tls_server_name     ? tls_server_name.error()
               : !ca_certificate      ? ca_certificate.error()
               : !client_certificate  ? client_certificate.error()
                                      : client_private_key.error();
    }

    std::optional<common::Endpoint> endpoint;
    if (endpoint_text_value->has_value()) {
        auto parsed = common::Endpoint::parse(**endpoint_text_value);
        if (!parsed) {
            return parsed.error();
        }
        endpoint = std::move(*parsed);
    }

    std::unique_lock credential_operation_lock{credential_operation_mutex_};
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto server = resolve_server(repository_, *identifier);
    if (!server) {
        return server.error();
    }
    auto tunnels = repository_.tunnels().list_by_server(server->id);
    if (!tunnels) {
        return tunnels.error();
    }

    const ServerRecord previous = *server;
    auto current_ca = load_credential(credentials_, previous.ca_credential_ref, "server CA");
    auto current_certificate =
        load_credential(credentials_, previous.client_certificate_ref, "client certificate");
    auto current_private_key =
        load_credential(credentials_, previous.client_private_key_ref, "client private key");
    if (!current_ca || !current_certificate || !current_private_key) {
        return !current_ca            ? current_ca.error()
               : !current_certificate ? current_certificate.error()
                                      : current_private_key.error();
    }

    common::SecureString prospective_ca;
    common::SecureString prospective_certificate;
    common::SecureString prospective_private_key;
    bool ca_changed = false;
    bool certificate_changed = false;
    bool private_key_changed = false;
    if (ca_certificate->has_value()) {
        auto parsed =
            parse_credential_material(*ca_certificate, common::SecureString{}, "server CA");
        if (!parsed) {
            return parsed.error();
        }
        ca_changed = !parsed->equals(*current_ca);
        prospective_ca = std::move(*parsed);
    } else {
        prospective_ca = std::move(*current_ca);
    }
    if (client_certificate->has_value()) {
        auto parsed = parse_credential_material(*client_certificate, common::SecureString{},
                                                "client certificate");
        if (!parsed) {
            return parsed.error();
        }
        certificate_changed = !parsed->equals(*current_certificate);
        prospective_certificate = std::move(*parsed);
    } else {
        prospective_certificate = std::move(*current_certificate);
    }
    if (client_private_key->has_value()) {
        auto parsed = parse_credential_material(*client_private_key, common::SecureString{},
                                                "client private key");
        if (!parsed) {
            return parsed.error();
        }
        private_key_changed = !parsed->equals(*current_private_key);
        prospective_private_key = std::move(*parsed);
    } else {
        prospective_private_key = std::move(*current_private_key);
    }
    if (prospective_certificate.empty() != prospective_private_key.empty()) {
        return Error{ErrorCode::invalid_argument,
                     "client certificate and private key must be configured together"};
    }
    auto validated_tls = protocol::make_client_tls_context({
        .ca_certificate_path = {},
        .ca_certificate_pem = prospective_ca.view(),
        .client_certificate_pem = prospective_certificate.view(),
        .client_private_key_pem = prospective_private_key.view(),
    });
    if (!validated_tls) {
        return validated_tls.error();
    }

    StagedCredentials staged{credentials_};
    if (ca_changed) {
        if (prospective_ca.empty()) {
            server->ca_credential_ref.reset();
        } else {
            server->ca_credential_ref =
                next_server_credential_key(previous, ServerCredentialKind::ca_certificate);
            auto stored = staged.put(*server->ca_credential_ref, prospective_ca.view());
            if (!stored) {
                return stored.error();
            }
        }
    }
    if (certificate_changed) {
        if (prospective_certificate.empty()) {
            server->client_certificate_ref.reset();
        } else {
            server->client_certificate_ref =
                next_server_credential_key(previous, ServerCredentialKind::client_certificate);
            auto stored =
                staged.put(*server->client_certificate_ref, prospective_certificate.view());
            if (!stored) {
                return stored.error();
            }
        }
    }
    if (private_key_changed) {
        if (prospective_private_key.empty()) {
            server->client_private_key_ref.reset();
        } else {
            server->client_private_key_ref =
                next_server_credential_key(previous, ServerCredentialKind::client_private_key);
            auto stored =
                staged.put(*server->client_private_key_ref, prospective_private_key.view());
            if (!stored) {
                return stored.error();
            }
        }
    }

    bool changed = false;
    bool transport_changed = ca_changed || certificate_changed || private_key_changed;
    changed = transport_changed;
    if (name->has_value() && server->name != **name) {
        server->name = **name;
        changed = true;
    }
    if (endpoint.has_value() && server->endpoint != *endpoint) {
        server->endpoint = std::move(*endpoint);
        changed = true;
        transport_changed = true;
    }
    if (tls_server_name->has_value() && server->tls_server_name != **tls_server_name) {
        server->tls_server_name = **tls_server_name;
        changed = true;
        transport_changed = true;
    }

    if (changed) {
        auto revision = next_revision(server->config_revision);
        if (!revision) {
            return revision.error();
        }
        server->config_revision = *revision;
        server->updated_at_unix_ms = update_time(server->updated_at_unix_ms);
        if (transport_changed) {
            server->actual_state =
                server->desired_state == ServerDesiredState::disabled
                    ? ServerActualState::disabled
                    : (server->credential_ref.has_value() ? ServerActualState::disconnected
                                                          : ServerActualState::not_authenticated);
            server->remote_server_id.reset();
            server->last_error_code.reset();
            server->last_error_message.reset();
            server->reconnect_attempt = 0U;
            server->latency_ms.reset();
        }
        auto updated = repository_.servers().update(*server, *transaction);
        if (!updated) {
            return updated.error();
        }
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    staged.release();
    const auto cleanup_kind = [this, &previous, &server](const ServerCredentialKind kind,
                                                         const bool material_changed) {
        if (!material_changed) {
            return;
        }
        const auto& old_reference = credential_reference(previous, kind);
        const auto& retained_reference = credential_reference(*server, kind);
        auto cleaned = cleanup_server_credential_kind(
            credentials_, server->id, kind,
            old_reference.has_value() ? std::optional<std::string_view>{*old_reference}
                                      : std::nullopt,
            retained_reference.has_value() ? std::optional<std::string_view>{*retained_reference}
                                           : std::nullopt);
        if (!cleaned) {
            common::log_warn("deferred cleanup of previous server TLS material",
                             {.component = "daemon.control",
                              .server_id = server->id.str(),
                              .error_code = cleaned.error().code()});
        }
    };
    cleanup_kind(ServerCredentialKind::ca_certificate, ca_changed);
    cleanup_kind(ServerCredentialKind::client_certificate, certificate_changed);
    cleanup_kind(ServerCredentialKind::client_private_key, private_key_changed);
    credential_operation_lock.unlock();
    if (changed) {
        notify_state_changed();
    }
    const std::size_t count = tunnel_counts(*tunnels)[server->id.str()];
    return Json{{"server", server_json(*server, count)}, {"changed", changed}};
}

Result<Json> ControlService::server_enable(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto server = resolve_server(repository_, *identifier);
    if (!server) {
        return server.error();
    }
    auto tunnels = repository_.tunnels().list_by_server(server->id);
    if (!tunnels) {
        return tunnels.error();
    }
    const bool changed = server->desired_state != ServerDesiredState::enabled;
    if (changed) {
        auto revision = next_revision(server->config_revision);
        if (!revision) {
            return revision.error();
        }
        server->desired_state = ServerDesiredState::enabled;
        server->actual_state = server->credential_ref.has_value()
                                   ? ServerActualState::disconnected
                                   : ServerActualState::not_authenticated;
        server->config_revision = *revision;
        server->last_error_code.reset();
        server->last_error_message.reset();
        server->reconnect_attempt = 0U;
        server->latency_ms.reset();
        server->updated_at_unix_ms = update_time(server->updated_at_unix_ms);
        auto updated = repository_.servers().update(*server, *transaction);
        if (!updated) {
            return updated.error();
        }
        for (auto& tunnel : *tunnels) {
            if (tunnel.desired_state == TunnelDesiredState::active) {
                tunnel.actual_state = TunnelActualState::pending;
                tunnel.updated_at_unix_ms = update_time(tunnel.updated_at_unix_ms);
                auto tunnel_updated = repository_.tunnels().update(tunnel, *transaction);
                if (!tunnel_updated) {
                    return tunnel_updated.error();
                }
            }
        }
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    if (changed) {
        notify_state_changed();
    }
    const std::size_t count = tunnel_counts(*tunnels)[server->id.str()];
    return Json{{"server", server_json(*server, count)}, {"changed", changed}};
}

Result<Json> ControlService::server_disable(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto server = resolve_server(repository_, *identifier);
    if (!server) {
        return server.error();
    }
    auto tunnels = repository_.tunnels().list_by_server(server->id);
    if (!tunnels) {
        return tunnels.error();
    }
    const bool changed = server->desired_state != ServerDesiredState::disabled;
    if (changed) {
        auto revision = next_revision(server->config_revision);
        if (!revision) {
            return revision.error();
        }
        server->desired_state = ServerDesiredState::disabled;
        server->actual_state = ServerActualState::disabled;
        server->config_revision = *revision;
        server->last_error_code.reset();
        server->last_error_message.reset();
        server->reconnect_attempt = 0U;
        server->latency_ms.reset();
        server->updated_at_unix_ms = update_time(server->updated_at_unix_ms);
        auto updated = repository_.servers().update(*server, *transaction);
        if (!updated) {
            return updated.error();
        }
        for (auto& tunnel : *tunnels) {
            if (tunnel.desired_state == TunnelDesiredState::active) {
                tunnel.actual_state = TunnelActualState::pending;
                tunnel.updated_at_unix_ms = update_time(tunnel.updated_at_unix_ms);
                auto tunnel_updated = repository_.tunnels().update(tunnel, *transaction);
                if (!tunnel_updated) {
                    return tunnel_updated.error();
                }
            }
        }
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    if (changed) {
        notify_state_changed();
    }
    const std::size_t count = tunnel_counts(*tunnels)[server->id.str()];
    return Json{{"server", server_json(*server, count)}, {"changed", changed}};
}

Result<Json> ControlService::server_logout(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    std::unique_lock credential_operation_lock{credential_operation_mutex_};
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto server = resolve_server(repository_, *identifier);
    if (!server) {
        return server.error();
    }
    auto tunnels = repository_.tunnels().list_by_server(server->id);
    if (!tunnels) {
        return tunnels.error();
    }
    const auto psk_ref = server->credential_ref;
    const auto certificate_ref = server->client_certificate_ref;
    const auto private_key_ref = server->client_private_key_ref;
    const auto logged_out_state = server->desired_state == ServerDesiredState::disabled
                                      ? ServerActualState::disabled
                                      : ServerActualState::not_authenticated;
    const bool changed = psk_ref.has_value() || certificate_ref.has_value() ||
                         private_key_ref.has_value() || server->actual_state != logged_out_state;
    if (changed) {
        auto revision = next_revision(server->config_revision);
        if (!revision) {
            return revision.error();
        }
        server->credential_ref.reset();
        server->client_certificate_ref.reset();
        server->client_private_key_ref.reset();
        server->remote_server_id.reset();
        server->actual_state = logged_out_state;
        server->config_revision = *revision;
        server->last_error_code.reset();
        server->last_error_message.reset();
        server->reconnect_attempt = 0U;
        server->latency_ms.reset();
        server->updated_at_unix_ms = update_time(server->updated_at_unix_ms);
        auto updated = repository_.servers().update(*server, *transaction);
        if (!updated) {
            return updated.error();
        }
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }

    auto psk_cleaned = cleanup_server_credentials(
        credentials_, server->id,
        psk_ref.has_value() ? std::optional<std::string_view>{*psk_ref} : std::nullopt);
    std::optional<Error> cleanup_error;
    if (!psk_cleaned) {
        cleanup_error = psk_cleaned.error();
    }
    for (const auto& [kind, reference] :
         {std::pair{ServerCredentialKind::client_certificate, certificate_ref},
          std::pair{ServerCredentialKind::client_private_key, private_key_ref}}) {
        auto removed = cleanup_server_credential_kind(
            credentials_, server->id, kind,
            reference.has_value() ? std::optional<std::string_view>{*reference} : std::nullopt);
        if (!removed && !cleanup_error.has_value()) {
            cleanup_error = removed.error();
        }
    }
    if (cleanup_error.has_value()) {
        common::log_warn("deferred credential cleanup after server logout",
                         {.component = "daemon.control",
                          .server_id = server->id.str(),
                          .error_code = cleanup_error->code()});
    }
    credential_operation_lock.unlock();
    if (changed) {
        notify_state_changed();
    }
    const std::size_t count = tunnel_counts(*tunnels)[server->id.str()];
    return Json{{"server", server_json(*server, count)}, {"changed", changed}};
}

Result<Json> ControlService::server_list(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto servers = repository_.servers().list();
    auto tunnels = repository_.tunnels().list();
    if (!servers) {
        return servers.error();
    }
    if (!tunnels) {
        return tunnels.error();
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    const auto counts = tunnel_counts(*tunnels);
    Json result = Json::array();
    for (const auto& server : *servers) {
        if (server.desired_state == ServerDesiredState::removed) {
            continue;
        }
        const auto count = counts.find(server.id.str());
        result.push_back(server_json(server, count == counts.end() ? 0U : count->second));
    }
    return Json{{"servers", std::move(result)}};
}

Result<Json> ControlService::server_inspect(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto server = resolve_server(repository_, *identifier);
    if (!server) {
        return server.error();
    }
    auto tunnels = repository_.tunnels().list_by_server(server->id);
    if (!tunnels) {
        return tunnels.error();
    }
    const std::size_t count = tunnel_counts(*tunnels)[server->id.str()];
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    return Json{{"server", server_json(*server, count)}};
}

Result<Json> ControlService::server_remove(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    std::unique_lock credential_operation_lock{credential_operation_mutex_};
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto server = resolve_server(repository_, *identifier);
    if (!server) {
        return server.error();
    }
    auto removed = repository_.servers().mark_removed(
        server->id, update_time(server->updated_at_unix_ms), *transaction);
    if (!removed) {
        return removed.error();
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }

    // The committed tombstone is the logical deletion boundary. It is durable as
    // part of the SQLite WAL even when a long-lived external reader prevents an
    // immediate checkpoint. Cleanup below is an idempotent saga that startup and
    // ServerManager reconciliation can finish after interruption.
    auto checkpointed = repository_.checkpoint();
    if (!checkpointed) {
        common::log_warn("deferred state checkpoint after server removal",
                         {.component = "daemon.control",
                          .server_id = server->id.str(),
                          .error_code = checkpointed.error().code()});
    }
    auto credentials_removed = cleanup_all_server_credentials(credentials_, *server);
    if (!credentials_removed) {
        common::log_warn("deferred credential cleanup after server removal",
                         {.component = "daemon.control",
                          .server_id = server->id.str(),
                          .error_code = credentials_removed.error().code()});
    } else {
        auto erased = repository_.servers().erase(server->id);
        if (!erased && erased.error().code() != ErrorCode::not_found) {
            common::log_warn("deferred state cleanup after server removal",
                             {.component = "daemon.control",
                              .server_id = server->id.str(),
                              .error_code = erased.error().code()});
        } else {
            checkpointed = repository_.checkpoint();
            if (!checkpointed) {
                common::log_warn("deferred final state checkpoint after server removal",
                                 {.component = "daemon.control",
                                  .server_id = server->id.str(),
                                  .error_code = checkpointed.error().code()});
            }
        }
    }
    credential_operation_lock.unlock();
    notify_state_changed();
    return Json{{"removed", Json{{"id", server->id.str()}, {"name", optional_json(server->name)}}}};
}

Result<Json> ControlService::tunnel_add(const ipc::Request& request) {
    if (auto valid =
            validate_params(request.params, {"server", "remote_port"},
                            {"local_host", "local_port", "remote_host", "name", "protocol"});
        !valid) {
        return valid.error();
    }
    auto server_identifier = required_string(request.params, "server");
    auto local_port = optional_port(request.params, "local_port");
    auto remote_port = required_port(request.params, "remote_port");
    auto local_host = optional_string(request.params, "local_host");
    auto remote_host = optional_string(request.params, "remote_host");
    auto name = optional_string(request.params, "name");
    auto protocol_text = optional_string(request.params, "protocol");
    if (!server_identifier) {
        return server_identifier.error();
    }
    if (!local_port) {
        return local_port.error();
    }
    if (!remote_port) {
        return remote_port.error();
    }
    if (!local_host) {
        return local_host.error();
    }
    if (!remote_host) {
        return remote_host.error();
    }
    if (!name) {
        return name.error();
    }
    if (!protocol_text) {
        return protocol_text.error();
    }
    auto tunnel_protocol = storage::tunnel_protocol_from_string(protocol_text->value_or("tcp"));
    if (!tunnel_protocol) {
        return tunnel_protocol.error();
    }
    if (*tunnel_protocol != storage::TunnelProtocol::socks5 && !local_port->has_value()) {
        return Error{ErrorCode::invalid_argument,
                     "local_port is required for tcp, udp, and p2p tunnels"};
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto server = resolve_server(repository_, *server_identifier);
    if (!server) {
        return server.error();
    }
    const std::string host = local_host->value_or("127.0.0.1");
    const std::uint16_t target_port = local_port->value_or(1U);
    const std::string bind_host = remote_host->value_or(
        *tunnel_protocol == storage::TunnelProtocol::socks5 ? "127.0.0.1" : "0.0.0.0");
    auto local_endpoint = common::Endpoint::parse(endpoint_text(host, target_port));
    auto remote_endpoint = common::Endpoint::parse(endpoint_text(bind_host, *remote_port));
    if (!local_endpoint) {
        return local_endpoint.error();
    }
    if (!remote_endpoint) {
        return remote_endpoint.error();
    }
    if (auto valid_binding = validate_protocol_binding(*tunnel_protocol, *remote_endpoint);
        !valid_binding) {
        return valid_binding.error();
    }
    auto id = common::Id::generate(common::IdKind::tunnel);
    if (!id) {
        return id.error();
    }
    const std::int64_t now = common::unix_milliseconds_now();
    std::optional<ErrorCode> initial_error_code = server->last_error_code;
    std::optional<std::string> initial_error_message = server->last_error_message;
    if (server->actual_state != ServerActualState::online && !server->credential_ref.has_value() &&
        !initial_error_code.has_value()) {
        initial_error_code = ErrorCode::not_authenticated;
        initial_error_message = "server credentials are not configured";
    }
    TunnelRecord tunnel{
        .id = std::move(*id),
        .name = std::move(*name),
        .server_id = server->id,
        .protocol = *tunnel_protocol,
        .local_endpoint = std::move(*local_endpoint),
        .remote_endpoint = std::move(*remote_endpoint),
        .desired_state = TunnelDesiredState::active,
        .actual_state = TunnelActualState::pending,
        .last_error_code = initial_error_code,
        .last_error_message = std::move(initial_error_message),
        .created_at_unix_ms = now,
        .updated_at_unix_ms = now,
        .last_synced_at_unix_ms = std::nullopt,
    };
    auto created = repository_.tunnels().create(tunnel, *transaction);
    if (!created) {
        return created.error();
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    notify_state_changed();
    const TunnelServerContext server_context{server->name, server->actual_state};
    return Json{{"tunnel", tunnel_json(tunnel, &server_context)}};
}

Result<Json> ControlService::tunnel_update(const ipc::Request& request) {
    if (auto valid = validate_params(
            request.params, {"identifier"},
            {"name", "local_host", "local_port", "remote_host", "remote_port", "protocol"});
        !valid) {
        return valid.error();
    }
    if (request.params.size() == 1U) {
        return Error{ErrorCode::invalid_argument, "tunnel update requires at least one field"};
    }
    auto identifier = required_string(request.params, "identifier");
    auto name = optional_nullable_string(request.params, "name");
    auto local_host = optional_string(request.params, "local_host");
    auto local_port = optional_port(request.params, "local_port");
    auto remote_host = optional_string(request.params, "remote_host");
    auto remote_port = optional_port(request.params, "remote_port");
    auto protocol_text = optional_string(request.params, "protocol");
    if (!identifier || !name || !local_host || !local_port || !remote_host || !remote_port ||
        !protocol_text) {
        return !identifier    ? identifier.error()
               : !name        ? name.error()
               : !local_host  ? local_host.error()
               : !local_port  ? local_port.error()
               : !remote_host ? remote_host.error()
               : !remote_port ? remote_port.error()
                              : protocol_text.error();
    }

    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto tunnel = resolve_tunnel(repository_, *identifier);
    if (!tunnel) {
        return tunnel.error();
    }
    auto server = repository_.servers().get_by_id(tunnel->server_id);
    if (!server) {
        return server.error();
    }

    const std::string next_local_host =
        local_host->has_value() ? **local_host : tunnel->local_endpoint.host();
    const std::uint16_t next_local_port =
        local_port->has_value() ? **local_port : tunnel->local_endpoint.port();
    const std::uint16_t next_remote_port =
        remote_port->has_value() ? **remote_port : tunnel->remote_endpoint.port();
    const std::string next_remote_host =
        remote_host->has_value() ? **remote_host : tunnel->remote_endpoint.host();
    storage::TunnelProtocol next_protocol = tunnel->protocol;
    if (protocol_text->has_value()) {
        auto parsed = storage::tunnel_protocol_from_string(**protocol_text);
        if (!parsed) {
            return parsed.error();
        }
        next_protocol = *parsed;
    }
    if (next_protocol != storage::TunnelProtocol::socks5 &&
        tunnel->protocol == storage::TunnelProtocol::socks5 && !local_port->has_value()) {
        return Error{ErrorCode::invalid_argument,
                     "local_port is required when changing a SOCKS5 tunnel to tcp, udp, or p2p"};
    }
    auto next_local = common::Endpoint::parse(endpoint_text(next_local_host, next_local_port));
    auto next_remote = common::Endpoint::parse(endpoint_text(next_remote_host, next_remote_port));
    if (!next_local) {
        return next_local.error();
    }
    if (!next_remote) {
        return next_remote.error();
    }
    if (auto valid_binding = validate_protocol_binding(next_protocol, *next_remote);
        !valid_binding) {
        return valid_binding.error();
    }

    bool changed = false;
    if (name->has_value() && tunnel->name != **name) {
        tunnel->name = **name;
        changed = true;
    }
    if (tunnel->local_endpoint != *next_local) {
        tunnel->local_endpoint = std::move(*next_local);
        changed = true;
    }
    if (tunnel->remote_endpoint != *next_remote) {
        tunnel->remote_endpoint = std::move(*next_remote);
        changed = true;
    }
    if (tunnel->protocol != next_protocol) {
        tunnel->protocol = next_protocol;
        changed = true;
    }
    if (changed) {
        auto revision = next_revision(tunnel->config_revision);
        if (!revision) {
            return revision.error();
        }
        tunnel->config_revision = *revision;
        tunnel->actual_state = tunnel->desired_state == TunnelDesiredState::active
                                   ? TunnelActualState::pending
                                   : TunnelActualState::disabled;
        tunnel->last_error_code.reset();
        tunnel->last_error_message.reset();
        tunnel->updated_at_unix_ms = update_time(tunnel->updated_at_unix_ms);
        auto updated = repository_.tunnels().update(*tunnel, *transaction);
        if (!updated) {
            return updated.error();
        }
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    if (changed) {
        notify_state_changed();
    }
    const TunnelServerContext server_context{server->name, server->actual_state};
    return Json{{"tunnel", tunnel_json(*tunnel, &server_context)}, {"changed", changed}};
}

Result<Json> ControlService::tunnel_enable(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto tunnel = resolve_tunnel(repository_, *identifier);
    if (!tunnel) {
        return tunnel.error();
    }
    auto server = repository_.servers().get_by_id(tunnel->server_id);
    if (!server) {
        return server.error();
    }
    const bool changed = tunnel->desired_state != TunnelDesiredState::active;
    if (changed) {
        auto revision = next_revision(tunnel->config_revision);
        if (!revision) {
            return revision.error();
        }
        tunnel->desired_state = TunnelDesiredState::active;
        tunnel->actual_state = TunnelActualState::pending;
        tunnel->config_revision = *revision;
        tunnel->last_error_code.reset();
        tunnel->last_error_message.reset();
        tunnel->updated_at_unix_ms = update_time(tunnel->updated_at_unix_ms);
        auto updated = repository_.tunnels().update(*tunnel, *transaction);
        if (!updated) {
            return updated.error();
        }
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    if (changed) {
        notify_state_changed();
    }
    const TunnelServerContext server_context{server->name, server->actual_state};
    return Json{{"tunnel", tunnel_json(*tunnel, &server_context)}, {"changed", changed}};
}

Result<Json> ControlService::tunnel_disable(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto tunnel = resolve_tunnel(repository_, *identifier);
    if (!tunnel) {
        return tunnel.error();
    }
    auto server = repository_.servers().get_by_id(tunnel->server_id);
    if (!server) {
        return server.error();
    }
    const bool changed = tunnel->desired_state != TunnelDesiredState::disabled;
    if (changed) {
        auto revision = next_revision(tunnel->config_revision);
        if (!revision) {
            return revision.error();
        }
        tunnel->desired_state = TunnelDesiredState::disabled;
        tunnel->actual_state = TunnelActualState::disabled;
        tunnel->config_revision = *revision;
        tunnel->last_error_code.reset();
        tunnel->last_error_message.reset();
        tunnel->updated_at_unix_ms = update_time(tunnel->updated_at_unix_ms);
        auto updated = repository_.tunnels().update(*tunnel, *transaction);
        if (!updated) {
            return updated.error();
        }
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    if (changed) {
        notify_state_changed();
    }
    const TunnelServerContext server_context{server->name, server->actual_state};
    return Json{{"tunnel", tunnel_json(*tunnel, &server_context)}, {"changed", changed}};
}

Result<Json> ControlService::tunnel_list(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}, {"server"}); !valid) {
        return valid.error();
    }
    auto server_identifier = optional_string(request.params, "server");
    if (!server_identifier) {
        return server_identifier.error();
    }

    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    std::optional<ServerRecord> selected_server;
    Result<std::vector<TunnelRecord>> tunnels = repository_.tunnels().list();
    if (server_identifier->has_value()) {
        auto server = resolve_server(repository_, **server_identifier);
        if (!server) {
            return server.error();
        }
        selected_server = std::move(*server);
        tunnels = repository_.tunnels().list_by_server(selected_server->id);
    }
    if (!tunnels) {
        return tunnels.error();
    }

    std::unordered_map<std::string, TunnelServerContext> server_contexts;
    if (selected_server.has_value()) {
        server_contexts.emplace(
            selected_server->id.str(),
            TunnelServerContext{selected_server->name, selected_server->actual_state});
    } else {
        auto servers = repository_.servers().list();
        if (!servers) {
            return servers.error();
        }
        for (const auto& server : *servers) {
            if (server.desired_state != ServerDesiredState::removed) {
                server_contexts.emplace(server.id.str(),
                                        TunnelServerContext{server.name, server.actual_state});
            }
        }
    }

    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }

    Json result = Json::array();
    for (const auto& tunnel : *tunnels) {
        if (tunnel.desired_state == TunnelDesiredState::removed) {
            continue;
        }
        const auto server = server_contexts.find(tunnel.server_id.str());
        result.push_back(
            tunnel_json(tunnel, server == server_contexts.end() ? nullptr : &server->second));
    }
    return Json{{"tunnels", std::move(result)}};
}

Result<Json> ControlService::tunnel_inspect(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto tunnel = resolve_tunnel(repository_, *identifier);
    if (!tunnel) {
        return tunnel.error();
    }
    auto server = repository_.servers().get_by_id(tunnel->server_id);
    if (!server) {
        return server.error();
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    const TunnelServerContext server_context{server->name, server->actual_state};
    return Json{{"tunnel", tunnel_json(*tunnel, &server_context)}};
}

Result<Json> ControlService::tunnel_remove(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    if (!identifier) {
        return identifier.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto tunnel = resolve_tunnel(repository_, *identifier);
    if (!tunnel) {
        return tunnel.error();
    }
    auto removed = repository_.tunnels().mark_removed(
        tunnel->id, update_time(tunnel->updated_at_unix_ms), *transaction);
    if (!removed) {
        return removed.error();
    }
    auto erased = repository_.tunnels().erase(tunnel->id, *transaction);
    if (!erased) {
        return erased.error();
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    auto checkpointed = repository_.checkpoint();
    if (!checkpointed) {
        common::log_warn("deferred state checkpoint after tunnel removal",
                         {.component = "daemon.control",
                          .server_id = tunnel->server_id.str(),
                          .tunnel_id = tunnel->id.str(),
                          .error_code = checkpointed.error().code()});
    }
    notify_state_changed();
    return Json{{"removed", Json{{"id", tunnel->id.str()}, {"name", optional_json(tunnel->name)}}}};
}

Result<Json> ControlService::config_export(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    DeclarativeConfig configuration{repository_, credentials_};
    return configuration.export_config();
}

Result<Json> ControlService::config_plan(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {"path"}, {"prune"}); !valid) {
        return valid.error();
    }
    auto path = required_string(request.params, "path");
    auto prune = optional_bool(request.params, "prune");
    if (!path) {
        return path.error();
    }
    if (!prune) {
        return prune.error();
    }
    DeclarativeConfig configuration{repository_, credentials_};
    return configuration.plan(*path, *prune);
}

Result<Json> ControlService::config_apply(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"path"}, {"prune"}); !valid) {
        return valid.error();
    }
    auto path = required_string(request.params, "path");
    auto prune = optional_bool(request.params, "prune");
    if (!path) {
        return path.error();
    }
    if (!prune) {
        return prune.error();
    }
    std::unique_lock credential_operation_lock{credential_operation_mutex_};
    DeclarativeConfig configuration{repository_, credentials_};
    auto applied = configuration.apply(*path, *prune);
    credential_operation_lock.unlock();
    if (!applied) {
        return applied.error();
    }
    const auto changed = applied->find("changed");
    if (changed != applied->end() && changed->is_number_unsigned() &&
        changed->get<std::uint64_t>() != 0U) {
        notify_state_changed();
    }
    return applied;
}

Result<Json> ControlService::status(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    auto servers = repository_.servers().list();
    auto tunnels = repository_.tunnels().list();
    if (!servers) {
        return servers.error();
    }
    if (!tunnels) {
        return tunnels.error();
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }

    std::map<std::string, std::size_t> server_states;
    for (const std::string_view state :
         {"not_authenticated", "disconnected", "connecting", "tls_handshake", "authenticating",
          "online", "backoff", "disabled", "error"}) {
        server_states.emplace(state, 0U);
    }
    std::map<std::string, std::size_t> tunnel_states;
    for (const std::string_view state :
         {"pending", "registering", "active", "failed", "removing", "disabled"}) {
        tunnel_states.emplace(state, 0U);
    }

    std::size_t server_total = 0U;
    for (const auto& server : *servers) {
        if (server.desired_state == ServerDesiredState::removed) {
            continue;
        }
        ++server_total;
        ++server_states[std::string{storage::to_string(server.actual_state)}];
    }
    std::size_t tunnel_total = 0U;
    for (const auto& tunnel : *tunnels) {
        if (tunnel.desired_state == TunnelDesiredState::removed) {
            continue;
        }
        ++tunnel_total;
        ++tunnel_states[std::string{storage::to_string(tunnel.actual_state)}];
    }

    Json server_state_json = Json::object();
    for (const auto& [state, count] : server_states) {
        server_state_json[state] = count;
    }
    Json tunnel_state_json = Json::object();
    for (const auto& [state, count] : tunnel_states) {
        tunnel_state_json[state] = count;
    }
    Json runtime = Json{
        {"sessions", Json{{"active", 0U}}},
        {"workers", Json{{"idle", 0U}, {"active", 0U}}},
        {"connections", Json{{"active", 0U}, {"pending", 0U}}},
        {"reconnects", 0U},
        {"quota_rejections", 0U},
        {"errors", 0U},
        {"throughput", Json{{"bytes_in", 0U}, {"bytes_out", 0U}}},
    };
    if (runtime_metrics_) {
        try {
            auto supplied = runtime_metrics_();
            if (supplied.is_object()) {
                for (auto iterator = supplied.begin(); iterator != supplied.end(); ++iterator) {
                    runtime[iterator.key()] = std::move(iterator.value());
                }
            }
        } catch (...) {
            runtime["provider_error"] = true;
        }
    }

    return Json{
        {"daemon", Json{{"state", "running"}, {"ipc_version", ipc::kProtocolVersion}}},
        {"servers", Json{{"total", server_total},
                         {"online", server_states["online"]},
                         {"states", std::move(server_state_json)}}},
        {"tunnels", Json{{"total", tunnel_total},
                         {"active", tunnel_states["active"]},
                         {"states", std::move(tunnel_state_json)}}},
        {"runtime", std::move(runtime)},
    };
}

Result<Json> ControlService::doctor(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {},
                                     {"backup_state", "backup_credentials", "restore_state",
                                      "restore_credentials", "checkpoint"});
        !valid) {
        return valid.error();
    }
    const auto read_path =
        [&request](const std::string_view field) -> Result<std::optional<std::string>> {
        return optional_string(request.params, field);
    };
    auto backup_state = read_path("backup_state");
    auto backup_credentials = read_path("backup_credentials");
    auto restore_state = read_path("restore_state");
    auto restore_credentials = read_path("restore_credentials");
    if (!backup_state || !backup_credentials || !restore_state || !restore_credentials) {
        return !backup_state         ? backup_state.error()
               : !backup_credentials ? backup_credentials.error()
               : !restore_state      ? restore_state.error()
                                     : restore_credentials.error();
    }
    const auto checkpoint = request.params.find("checkpoint");
    if (checkpoint != request.params.end() && !checkpoint->is_boolean()) {
        return Error{ErrorCode::invalid_argument, "checkpoint must be a boolean"};
    }

    auto* const sqlite_credentials = dynamic_cast<storage::SqliteCredentialStore*>(&credentials_);
    if (sqlite_credentials == nullptr) {
        return Error{ErrorCode::unsupported_version,
                     "database doctor is unavailable for this credential store"};
    }

    // A restore spanning two SQLite files cannot use one database transaction.
    // Validate every requested source before changing either live database;
    // each subsequent online-backup copy remains atomic for its own database.
    if (restore_state->has_value()) {
        auto validated = repository_.validate_restore_source(**restore_state);
        if (!validated) {
            return validated.error();
        }
    }
    if (restore_credentials->has_value()) {
        auto validated = sqlite_credentials->validate_restore_source(**restore_credentials);
        if (!validated) {
            return validated.error();
        }
    }

    bool restored_any = false;
    if (restore_state->has_value()) {
        auto restored = repository_.restore_from(**restore_state);
        if (!restored) {
            return restored.error();
        }
        restored_any = true;
    }
    if (restore_credentials->has_value()) {
        auto restored = sqlite_credentials->restore_from(**restore_credentials);
        if (!restored) {
            if (restored_any) {
                notify_state_changed();
            }
            return restored.error();
        }
        restored_any = true;
    }
    if (restored_any) {
        notify_state_changed();
    }
    if (checkpoint != request.params.end() && checkpoint->get<bool>()) {
        auto checkpointed = repository_.checkpoint();
        if (!checkpointed) {
            return checkpointed.error();
        }
    }
    Json actions = Json::object();
    if (restore_state->has_value()) {
        actions["restore_state"] = **restore_state;
    }
    if (restore_credentials->has_value()) {
        actions["restore_credentials"] = **restore_credentials;
    }
    if (restore_state->has_value() || restore_credentials->has_value()) {
        actions["restore_consistency"] =
            restore_state->has_value() && restore_credentials->has_value()
                ? "prevalidated_per_database"
                : "single_database_atomic";
    }
    if (backup_state->has_value()) {
        auto backed_up = repository_.backup_to(**backup_state);
        if (!backed_up) {
            return backed_up.error();
        }
        actions["backup_state"] = **backup_state;
    }
    if (backup_credentials->has_value()) {
        auto backed_up = sqlite_credentials->backup_to(**backup_credentials);
        if (!backed_up) {
            return backed_up.error();
        }
        actions["backup_credentials"] = **backup_credentials;
    }

    auto state = repository_.diagnostics();
    auto credentials = sqlite_credentials->diagnostics();
    if (!state) {
        return state.error();
    }
    if (!credentials) {
        return credentials.error();
    }
    const bool healthy = state->schema_valid && state->integrity_ok && credentials->schema_valid &&
                         credentials->integrity_ok;
    return Json{{"ok", healthy},
                {"state_db", diagnostics_json(*state)},
                {"credentials_db", diagnostics_json(*credentials)},
                {"actions", std::move(actions)}};
}

Result<Json> ControlService::health(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    auto state = repository_.diagnostics();
    if (!state) {
        return state.error();
    }
    auto* const sqlite_credentials =
        dynamic_cast<const storage::SqliteCredentialStore*>(&credentials_);
    if (sqlite_credentials == nullptr) {
        return Error{ErrorCode::unsupported_version, "credential health is unavailable"};
    }
    auto credentials = sqlite_credentials->diagnostics();
    if (!credentials) {
        return credentials.error();
    }
    const bool healthy = state->schema_valid && state->integrity_ok && credentials->schema_valid &&
                         credentials->integrity_ok;
    return Json{{"status", healthy ? "ok" : "degraded"},
                {"state_db", state->schema_valid && state->integrity_ok},
                {"credentials_db", credentials->schema_valid && credentials->integrity_ok}};
}

Result<Json> ControlService::readiness(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    auto state = repository_.diagnostics();
    if (!state) {
        return Json{{"ready", false}, {"reason", "state_database_unavailable"}};
    }
    auto* const sqlite_credentials =
        dynamic_cast<const storage::SqliteCredentialStore*>(&credentials_);
    if (sqlite_credentials == nullptr) {
        return Json{{"ready", false}, {"reason", "credential_store_unavailable"}};
    }
    auto credentials = sqlite_credentials->diagnostics();
    if (!credentials) {
        return Json{{"ready", false}, {"reason", "credential_database_unavailable"}};
    }
    const bool ready = state->schema_valid && state->integrity_ok && credentials->schema_valid &&
                       credentials->integrity_ok;
    return Json{{"ready", ready},
                {"reason", ready ? Json(nullptr) : Json("database_check_failed")}};
}

Result<Json> ControlService::metrics(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    if (!runtime_metrics_) {
        return Json::object();
    }
    try {
        auto supplied = runtime_metrics_();
        return supplied.is_object() ? supplied : Json::object();
    } catch (...) {
        return Error{ErrorCode::internal_error, "metrics provider failed"};
    }
}

Result<Json> ControlService::reload(const ipc::Request& request) const {
    if (auto valid = validate_params(request.params, {}); !valid) {
        return valid.error();
    }
    if (!reload_handler_) {
        return Error{ErrorCode::unsupported_version, "reload is not supported by this daemon"};
    }
    auto reloaded = reload_handler_();
    if (!reloaded) {
        return reloaded.error();
    }
    return Json{{"reloaded", true}};
}

} // namespace minitun::daemon
