#include <minitun/daemon/control_service.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/logging.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/daemon/credential_keys.hpp>
#include <minitun/ipc/dispatcher.hpp>
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
        {"credential_configured", server.credential_ref.has_value()},
        {"remote_server_id", optional_json(server.remote_server_id)},
        {"desired_state", std::string{storage::to_string(server.desired_state)}},
        {"actual_state", std::string{storage::to_string(server.actual_state)}},
        {"last_error", last_error_json(server.last_error_code, server.last_error_message)},
        {"reconnect_attempt", server.reconnect_attempt},
        {"latency_ms", optional_json(server.latency_ms)},
        {"tunnel_count", tunnel_count},
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
    registered.reserve(16U);
    const auto add = [&dispatcher, &registered](std::string method,
                                                ipc::MethodHandler handler) -> Result<void> {
        std::string method_copy = method;
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
        std::pair{"server.add", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_add(request);
                  }}},
        std::pair{"server.login", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return server_login(request);
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
        std::pair{"tun.list", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_list(request);
                  }}},
        std::pair{"tun.inspect", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_inspect(request);
                  }}},
        std::pair{"tun.remove", ipc::MethodHandler{[this](const ipc::Request& request) {
                      return tunnel_remove(request);
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
    };
    auto created = repository_.servers().create(server);
    if (!created) {
        return created.error();
    }
    notify_state_changed();
    return Json{{"server", server_json(server, 0U)}};
}

Result<Json> ControlService::server_login(const ipc::Request& request) {
    if (auto valid = validate_params(request.params, {"identifier", "token"}); !valid) {
        return valid.error();
    }
    auto identifier = required_string(request.params, "identifier");
    auto token_text = required_string(request.params, "token");
    if (!identifier) {
        return identifier.error();
    }
    if (!token_text) {
        return token_text.error();
    }
    const StringScrubber token_scrubber{*token_text};
    if (token_text->empty() || token_text->size() > storage::kMaxCredentialSecretBytes ||
        token_text->find('\0') != std::string::npos) {
        return Error{ErrorCode::invalid_argument, "token is outside its accepted byte-length"};
    }
    common::SecureString token{*token_text};
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
    server->credential_ref = key;
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
    auto stored = credentials_.put(key, token.view());
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
    auto credentials_removed =
        cleanup_server_credentials(credentials_, server->id,
                                   server->credential_ref.has_value()
                                       ? std::optional<std::string_view>{*server->credential_ref}
                                       : std::nullopt);
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
    if (auto valid = validate_params(request.params, {"server", "local_port", "remote_port"},
                                     {"local_host", "name"});
        !valid) {
        return valid.error();
    }
    auto server_identifier = required_string(request.params, "server");
    auto local_port = required_port(request.params, "local_port");
    auto remote_port = required_port(request.params, "remote_port");
    auto local_host = optional_string(request.params, "local_host");
    auto name = optional_string(request.params, "name");
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
    if (!name) {
        return name.error();
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
    auto local_endpoint = common::Endpoint::parse(endpoint_text(host, *local_port));
    auto remote_endpoint = common::Endpoint::parse(endpoint_text("0.0.0.0", *remote_port));
    if (!local_endpoint) {
        return local_endpoint.error();
    }
    if (!remote_endpoint) {
        return remote_endpoint.error();
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
        .protocol = storage::TunnelProtocol::tcp,
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
