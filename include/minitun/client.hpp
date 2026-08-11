#pragma once

#include <minitun/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace minitun {

enum class ClientErrorCode : std::uint8_t {
    ok = MINITUN_ERROR_OK,
    invalid_argument = MINITUN_ERROR_INVALID_ARGUMENT,
    not_found = MINITUN_ERROR_NOT_FOUND,
    already_exists = MINITUN_ERROR_ALREADY_EXISTS,
    permission_denied = MINITUN_ERROR_PERMISSION_DENIED,
    not_authenticated = MINITUN_ERROR_NOT_AUTHENTICATED,
    authentication_failed = MINITUN_ERROR_AUTHENTICATION_FAILED,
    connection_failed = MINITUN_ERROR_CONNECTION_FAILED,
    connection_timeout = MINITUN_ERROR_CONNECTION_TIMEOUT,
    remote_port_in_use = MINITUN_ERROR_REMOTE_PORT_IN_USE,
    local_connect_failed = MINITUN_ERROR_LOCAL_CONNECT_FAILED,
    protocol = MINITUN_ERROR_PROTOCOL,
    resource_exhausted = MINITUN_ERROR_RESOURCE_EXHAUSTED,
    database = MINITUN_ERROR_DATABASE,
    tls = MINITUN_ERROR_TLS,
    ipc = MINITUN_ERROR_IPC,
    unsupported_version = MINITUN_ERROR_UNSUPPORTED_VERSION,
    internal = MINITUN_ERROR_INTERNAL,
};

struct ClientError final {
    ClientErrorCode code{ClientErrorCode::internal};
    std::string message;
};

template <typename T> class Result final {
  public:
    Result(T value) : value_(std::move(value)) {}
    Result(ClientError error) : value_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(value_); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] T& value() & { return std::get<T>(value_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(value_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(value_)); }
    [[nodiscard]] ClientError& error() & { return std::get<ClientError>(value_); }
    [[nodiscard]] const ClientError& error() const& { return std::get<ClientError>(value_); }

  private:
    std::variant<T, ClientError> value_;
};

struct Identity final {
    std::string client_id{};
};

struct Status final {
    std::uint64_t server_total{0};
    std::uint64_t server_online{0};
    std::uint64_t tunnel_total{0};
    std::uint64_t tunnel_active{0};
    std::uint64_t sessions_active{0};
    std::uint64_t workers_idle{0};
    std::uint64_t workers_active{0};
    std::uint64_t connections_active{0};
};

struct Server final {
    std::string id{};
    std::optional<std::string> name{std::nullopt};
    std::string endpoint{};
    std::optional<std::string> tls_server_name{std::nullopt};
    std::string desired_state{};
    std::string actual_state{};
    std::uint64_t config_revision{0};
    bool credential_configured{false};
    bool ca_configured{false};
    bool client_certificate_configured{false};
    bool managed_by_config{false};
};

template <typename T> struct UpdateField final {
    bool specified{false};
    std::optional<T> value{std::nullopt};

    [[nodiscard]] static UpdateField set(T value) {
        return {true, std::optional<T>{std::move(value)}};
    }
    [[nodiscard]] static UpdateField clear() { return {true, std::nullopt}; }
};

struct ServerUpdate final {
    std::string identifier;
    UpdateField<std::string> name;
    UpdateField<std::string> endpoint;
    UpdateField<std::string> tls_server_name;
    UpdateField<std::string> ca_file;
    UpdateField<std::string> client_certificate_file;
    UpdateField<std::string> client_private_key_file;
};

enum class ServerAction : std::uint8_t {
    enable,
    disable,
    logout,
    remove,
};

struct Tunnel final {
    std::string id{};
    std::optional<std::string> name{std::nullopt};
    std::string server_id{};
    std::string local_endpoint{};
    std::string remote_endpoint{};
    std::string desired_state{};
    std::string actual_state{};
    std::uint64_t config_revision{0};
    bool managed_by_config{false};
};

struct TunnelCreate final {
    std::string server;
    std::optional<std::string> name;
    std::string local_host{"127.0.0.1"};
    std::uint16_t local_port{0};
    std::uint16_t remote_port{0};
    std::string protocol{"tcp"};
    std::optional<std::string> remote_host{std::nullopt};
};

struct TunnelUpdate final {
    std::string identifier;
    UpdateField<std::string> name;
    UpdateField<std::string> local_host;
    UpdateField<std::uint16_t> local_port;
    UpdateField<std::uint16_t> remote_port;
    UpdateField<std::string> protocol;
    UpdateField<std::string> remote_host;
};

enum class TunnelAction : std::uint8_t {
    enable,
    disable,
    remove,
};

enum class ConfigActionKind : std::uint8_t {
    create,
    update,
    disable,
    remove,
};

enum class ConfigResourceKind : std::uint8_t {
    server,
    tunnel,
};

struct ConfigAction final {
    ConfigActionKind action;
    ConfigResourceKind resource;
    std::optional<std::string> id;
    std::optional<std::string> name;
};

struct ConfigPlan final {
    std::vector<ConfigAction> actions;
    bool prune{false};
};

struct ConfigSnapshot final {
    std::vector<Server> servers;
    std::vector<Tunnel> tunnels;
};

struct Diagnostics final {
    bool healthy{false};
    bool ready{false};
    bool state_database_ok{false};
    bool credential_database_ok{false};
};

class Client final {
  public:
    [[nodiscard]] static Result<Client> create(std::string socket_path = {}) {
        minitun_client_options options{sizeof(options),
                                       socket_path.empty() ? nullptr : socket_path.c_str()};
        minitun_client* handle = nullptr;
        minitun_error* error = nullptr;
        if (minitun_client_create(&options, &handle, &error) != 0) {
            return take_error(error);
        }
        return Client{handle};
    }

    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) noexcept = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] Result<Identity> identity() const {
        minitun_identity raw{};
        minitun_error* error = nullptr;
        if (minitun_client_identity_get(handle_.get(), &raw, &error) != 0) {
            return take_error(error);
        }
        Identity result{safe_string(raw.client_id)};
        minitun_identity_free(&raw);
        return result;
    }

    [[nodiscard]] Result<Status> status() const {
        minitun_status raw{};
        minitun_error* error = nullptr;
        if (minitun_client_status_get(handle_.get(), &raw, &error) != 0) {
            return take_error(error);
        }
        return Status{raw.server_total,   raw.server_online,     raw.tunnel_total,
                      raw.tunnel_active,  raw.sessions_active,   raw.workers_idle,
                      raw.workers_active, raw.connections_active};
    }

    [[nodiscard]] Result<Server>
    create_server(std::string endpoint, std::optional<std::string> name = std::nullopt) const {
        minitun_server_create_request request{sizeof(request), endpoint.c_str(),
                                              name ? name->c_str() : nullptr};
        minitun_server_info raw{};
        minitun_error* error = nullptr;
        if (minitun_client_server_create(handle_.get(), &request, &raw, &error) != 0) {
            return take_error(error);
        }
        return take_server(raw);
    }

    [[nodiscard]] Result<Server> login_server(std::string identifier, std::string psk) const {
        minitun_server_info raw{};
        minitun_error* error = nullptr;
        const int status = minitun_client_server_login(handle_.get(), identifier.c_str(),
                                                       psk.c_str(), &raw, &error);
        std::fill(psk.begin(), psk.end(), '\0');
        if (status != 0) {
            return take_error(error);
        }
        return take_server(raw);
    }

    [[nodiscard]] Result<Server> update_server(const ServerUpdate& update) const {
        std::uint32_t mask = 0U;
        if (update.name.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_SERVER_UPDATE_NAME);
        }
        if (update.endpoint.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_SERVER_UPDATE_ENDPOINT);
        }
        if (update.tls_server_name.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_SERVER_UPDATE_TLS_SERVER_NAME);
        }
        if (update.ca_file.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_SERVER_UPDATE_CA_FILE);
        }
        if (update.client_certificate_file.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_SERVER_UPDATE_CLIENT_CERT_FILE);
        }
        if (update.client_private_key_file.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_SERVER_UPDATE_CLIENT_KEY_FILE);
        }
        minitun_server_update_request request{
            sizeof(request),
            update.identifier.c_str(),
            mask,
            pointer(update.name),
            pointer(update.endpoint),
            pointer(update.tls_server_name),
            pointer(update.ca_file),
            pointer(update.client_certificate_file),
            pointer(update.client_private_key_file),
        };
        minitun_server_info raw{};
        minitun_error* error = nullptr;
        if (minitun_client_server_update(handle_.get(), &request, &raw, &error) != 0) {
            return take_error(error);
        }
        return take_server(raw);
    }

    [[nodiscard]] Result<Server> execute_server(std::string identifier,
                                                const ServerAction action) const {
        minitun_server_info raw{};
        minitun_error* error = nullptr;
        const auto native = action == ServerAction::enable    ? MINITUN_SERVER_ENABLE
                            : action == ServerAction::disable ? MINITUN_SERVER_DISABLE
                            : action == ServerAction::logout  ? MINITUN_SERVER_LOGOUT
                                                              : MINITUN_SERVER_REMOVE;
        if (minitun_client_server_execute(handle_.get(), identifier.c_str(), native, &raw,
                                          &error) != 0) {
            return take_error(error);
        }
        if (action == ServerAction::remove) {
            return Server{.id = std::move(identifier), .desired_state = "removed"};
        }
        return take_server(raw);
    }

    [[nodiscard]] Result<std::vector<Server>> servers() const {
        minitun_server_list raw{};
        minitun_error* error = nullptr;
        if (minitun_client_server_list(handle_.get(), &raw, &error) != 0) {
            return take_error(error);
        }
        std::vector<Server> result;
        result.reserve(raw.size);
        for (std::size_t index = 0U; index < raw.size; ++index) {
            result.push_back(copy_server(raw.items[index]));
        }
        minitun_server_list_free(&raw);
        return result;
    }

    [[nodiscard]] Result<Tunnel> create_tunnel(const TunnelCreate& create) const {
        minitun_tunnel_create_request request{
            sizeof(request),
            create.server.c_str(),
            create.name ? create.name->c_str() : nullptr,
            create.local_host.c_str(),
            create.local_port,
            create.remote_port,
            create.protocol.c_str(),
            create.remote_host ? create.remote_host->c_str() : nullptr,
        };
        minitun_tunnel_info raw{};
        minitun_error* error = nullptr;
        if (minitun_client_tunnel_create(handle_.get(), &request, &raw, &error) != 0) {
            return take_error(error);
        }
        return take_tunnel(raw);
    }

    [[nodiscard]] Result<Tunnel> update_tunnel(const TunnelUpdate& update) const {
        std::uint32_t mask = 0U;
        if (update.name.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_TUNNEL_UPDATE_NAME);
        }
        if (update.local_host.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_TUNNEL_UPDATE_LOCAL_HOST);
        }
        if (update.local_port.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_TUNNEL_UPDATE_LOCAL_PORT);
        }
        if (update.remote_port.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_TUNNEL_UPDATE_REMOTE_PORT);
        }
        if (update.protocol.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_TUNNEL_UPDATE_PROTOCOL);
        }
        if (update.remote_host.specified) {
            mask |= static_cast<std::uint32_t>(MINITUN_TUNNEL_UPDATE_REMOTE_HOST);
        }
        minitun_tunnel_update_request request{
            sizeof(request),
            update.identifier.c_str(),
            mask,
            pointer(update.name),
            pointer(update.local_host),
            update.local_port.value.value_or(0U),
            update.remote_port.value.value_or(0U),
            pointer(update.protocol),
            pointer(update.remote_host),
        };
        minitun_tunnel_info raw{};
        minitun_error* error = nullptr;
        if (minitun_client_tunnel_update(handle_.get(), &request, &raw, &error) != 0) {
            return take_error(error);
        }
        return take_tunnel(raw);
    }

    [[nodiscard]] Result<Tunnel> execute_tunnel(std::string identifier,
                                                const TunnelAction action) const {
        minitun_tunnel_info raw{};
        minitun_error* error = nullptr;
        const auto native = action == TunnelAction::enable    ? MINITUN_TUNNEL_ENABLE
                            : action == TunnelAction::disable ? MINITUN_TUNNEL_DISABLE
                                                              : MINITUN_TUNNEL_REMOVE;
        if (minitun_client_tunnel_execute(handle_.get(), identifier.c_str(), native, &raw,
                                          &error) != 0) {
            return take_error(error);
        }
        if (action == TunnelAction::remove) {
            return Tunnel{.id = std::move(identifier), .desired_state = "removed"};
        }
        return take_tunnel(raw);
    }

    [[nodiscard]] Result<std::vector<Tunnel>>
    tunnels(const std::optional<std::string>& server = std::nullopt) const {
        minitun_tunnel_list raw{};
        minitun_error* error = nullptr;
        if (minitun_client_tunnel_list(handle_.get(), server ? server->c_str() : nullptr, &raw,
                                       &error) != 0) {
            return take_error(error);
        }
        std::vector<Tunnel> result;
        result.reserve(raw.size);
        for (std::size_t index = 0U; index < raw.size; ++index) {
            result.push_back(copy_tunnel(raw.items[index]));
        }
        minitun_tunnel_list_free(&raw);
        return result;
    }

    [[nodiscard]] Result<ConfigPlan> config_plan(std::string path, const bool prune = false) const {
        return run_config(path, prune, false);
    }

    [[nodiscard]] Result<ConfigPlan> config_apply(std::string path,
                                                  const bool prune = false) const {
        return run_config(path, prune, true);
    }

    [[nodiscard]] Result<ConfigSnapshot> config_export() const {
        minitun_config_snapshot raw{};
        minitun_error* error = nullptr;
        if (minitun_client_config_export(handle_.get(), &raw, &error) != 0) {
            return take_error(error);
        }
        ConfigSnapshot result;
        result.servers.reserve(raw.servers.size);
        result.tunnels.reserve(raw.tunnels.size);
        for (std::size_t index = 0U; index < raw.servers.size; ++index) {
            result.servers.push_back(copy_server(raw.servers.items[index]));
        }
        for (std::size_t index = 0U; index < raw.tunnels.size; ++index) {
            result.tunnels.push_back(copy_tunnel(raw.tunnels.items[index]));
        }
        minitun_config_snapshot_free(&raw);
        return result;
    }

    [[nodiscard]] Result<Diagnostics> diagnostics() const {
        minitun_diagnostics raw{};
        minitun_error* error = nullptr;
        if (minitun_client_diagnostics_get(handle_.get(), &raw, &error) != 0) {
            return take_error(error);
        }
        return Diagnostics{raw.healthy != 0U, raw.ready != 0U, raw.state_database_ok != 0U,
                           raw.credential_database_ok != 0U};
    }

  private:
    struct Deleter final {
        void operator()(minitun_client* value) const noexcept { minitun_client_destroy(value); }
    };

    explicit Client(minitun_client* handle) noexcept : handle_(handle) {}

    [[nodiscard]] static ClientError take_error(minitun_error* error) {
        ClientError result{
            error != nullptr ? static_cast<ClientErrorCode>(error->code)
                             : ClientErrorCode::internal,
            error != nullptr && error->message != nullptr ? error->message : "local SDK error",
        };
        minitun_error_free(error);
        return result;
    }

    [[nodiscard]] static std::string safe_string(const char* value) {
        return value != nullptr ? std::string{value} : std::string{};
    }

    [[nodiscard]] static std::optional<std::string> optional_string(const char* value) {
        return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
    }

    [[nodiscard]] static Server copy_server(const minitun_server_info& value) {
        return {safe_string(value.id),
                optional_string(value.name),
                safe_string(value.endpoint),
                optional_string(value.tls_server_name),
                safe_string(value.desired_state),
                safe_string(value.actual_state),
                value.config_revision,
                value.credential_configured != 0U,
                value.ca_configured != 0U,
                value.client_certificate_configured != 0U,
                value.managed_by_config != 0U};
    }

    [[nodiscard]] static Result<Server> take_server(minitun_server_info& value) {
        Server result = copy_server(value);
        minitun_server_info_free(&value);
        return result;
    }

    [[nodiscard]] static Tunnel copy_tunnel(const minitun_tunnel_info& value) {
        return {safe_string(value.id),
                optional_string(value.name),
                safe_string(value.server_id),
                safe_string(value.local_endpoint),
                safe_string(value.remote_endpoint),
                safe_string(value.desired_state),
                safe_string(value.actual_state),
                value.config_revision,
                value.managed_by_config != 0U};
    }

    [[nodiscard]] static Result<Tunnel> take_tunnel(minitun_tunnel_info& value) {
        Tunnel result = copy_tunnel(value);
        minitun_tunnel_info_free(&value);
        return result;
    }

    [[nodiscard]] static const char* pointer(const UpdateField<std::string>& field) noexcept {
        return field.value ? field.value->c_str() : nullptr;
    }

    [[nodiscard]] Result<ConfigPlan> run_config(const std::string& path, const bool prune,
                                                const bool apply) const {
        minitun_config_plan_result raw{};
        minitun_error* error = nullptr;
        const int status =
            apply ? minitun_client_config_apply(handle_.get(), path.c_str(), prune, &raw, &error)
                  : minitun_client_config_plan(handle_.get(), path.c_str(), prune, &raw, &error);
        if (status != 0) {
            return take_error(error);
        }
        ConfigPlan result;
        result.prune = raw.prune != 0U;
        result.actions.reserve(raw.size);
        for (std::size_t index = 0U; index < raw.size; ++index) {
            const auto& action = raw.actions[index];
            const auto kind = action.action == MINITUN_CONFIG_CREATE    ? ConfigActionKind::create
                              : action.action == MINITUN_CONFIG_UPDATE  ? ConfigActionKind::update
                              : action.action == MINITUN_CONFIG_DISABLE ? ConfigActionKind::disable
                                                                        : ConfigActionKind::remove;
            result.actions.push_back({kind,
                                      action.resource == MINITUN_CONFIG_SERVER
                                          ? ConfigResourceKind::server
                                          : ConfigResourceKind::tunnel,
                                      optional_string(action.id), optional_string(action.name)});
        }
        minitun_config_plan_result_free(&raw);
        return result;
    }

    std::unique_ptr<minitun_client, Deleter> handle_;
};

} // namespace minitun
