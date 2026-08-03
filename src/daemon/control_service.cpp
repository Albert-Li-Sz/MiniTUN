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
                               const std::optional<std::string>& server_name) {
    return Json{
        {"id", tunnel.id.str()},
        {"name", optional_json(tunnel.name)},
        {"server_id", tunnel.server_id.str()},
        {"server_name", optional_json(server_name)},
        {"protocol", std::string{storage::to_string(tunnel.protocol)}},
        {"local_endpoint", tunnel.local_endpoint.to_string()},
        {"remote_endpoint", tunnel.remote_endpoint.to_string()},
        {"desired_state", std::string{storage::to_string(tunnel.desired_state)}},
        {"actual_state", std::string{storage::to_string(tunnel.actual_state)}},
        {"last_error", last_error_json(tunnel.last_error_code, tunnel.last_error_message)},
        {"created_at", tunnel.created_at_unix_ms},
        {"updated_at", tunnel.updated_at_unix_ms},
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

} // namespace

ControlService::ControlService(storage::StateRepository& repository,
                               storage::CredentialStore& credentials) noexcept
    : repository_(repository), credentials_(credentials) {}

common::Result<void> ControlService::register_handlers(ipc::Dispatcher& dispatcher) {
    std::vector<std::string> registered;
    registered.reserve(11U);
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
    if (previous_key.has_value()) {
        auto removed = credentials_.remove(*previous_key);
        if (!removed) {
            // The committed state references the new credential. Retaining the old
            // slot is safe and the next alternating login or startup recovery will
            // retry its removal.
            common::log_warn("deferred cleanup of the previous server credential",
                             {.component = "daemon.control",
                              .server_id = server->id.str(),
                              .error_code = removed.error().code()});
        }
    }
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
    bool credentials_removed = true;
    const auto remove_credential = [this, &server,
                                    &credentials_removed](const std::string_view key) {
        auto result = credentials_.remove(key);
        if (!result) {
            credentials_removed = false;
            common::log_warn("deferred credential cleanup after server removal",
                             {.component = "daemon.control",
                              .server_id = server->id.str(),
                              .error_code = result.error().code()});
        }
    };
    if (server->credential_ref.has_value()) {
        remove_credential(*server->credential_ref);
    }
    for (const auto& key : managed_credential_keys(server->id)) {
        if (!server->credential_ref.has_value() || *server->credential_ref != key) {
            remove_credential(key);
        }
    }
    if (credentials_removed) {
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
    };
    auto created = repository_.tunnels().create(tunnel, *transaction);
    if (!created) {
        return created.error();
    }
    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    return Json{{"tunnel", tunnel_json(tunnel, server->name)}};
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

    std::unordered_map<std::string, std::optional<std::string>> server_names;
    if (selected_server.has_value()) {
        server_names.emplace(selected_server->id.str(), selected_server->name);
    } else {
        auto servers = repository_.servers().list();
        if (!servers) {
            return servers.error();
        }
        for (const auto& server : *servers) {
            if (server.desired_state != ServerDesiredState::removed) {
                server_names.emplace(server.id.str(), server.name);
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
        const auto server_name = server_names.find(tunnel.server_id.str());
        result.push_back(tunnel_json(tunnel, server_name == server_names.end()
                                                 ? std::optional<std::string>{}
                                                 : server_name->second));
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
    return Json{{"tunnel", tunnel_json(*tunnel, server->name)}};
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
    return Json{
        {"daemon", Json{{"state", "running"}, {"ipc_version", ipc::kProtocolVersion}}},
        {"servers", Json{{"total", server_total},
                         {"online", server_states["online"]},
                         {"states", std::move(server_state_json)}}},
        {"tunnels", Json{{"total", tunnel_total},
                         {"active", tunnel_states["active"]},
                         {"states", std::move(tunnel_state_json)}}},
    };
}

} // namespace minitun::daemon
