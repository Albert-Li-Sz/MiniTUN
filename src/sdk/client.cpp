#include <minitun/client.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/ipc/local_client.hpp>
#include <minitun/ipc/protocol.hpp>

struct minitun_client {
    std::string socket_path;
};

namespace {

using minitun::common::Error;
using minitun::common::ErrorCode;
using minitun::common::Result;
using minitun::ipc::Json;

constexpr std::size_t kMaximumInputBytes = 64U * 1024U;
constexpr std::size_t kMaximumPathBytes = 4'096U;

void erase_json(Json& value) noexcept {
    try {
        if (value.is_string()) {
            auto& text = value.get_ref<std::string&>();
            minitun::common::secure_erase_memory(text.data(), text.size());
            text.clear();
        } else if (value.is_array() || value.is_object()) {
            for (auto& child : value) {
                erase_json(child);
            }
        }
    } catch (...) {
    }
}

class JsonScrubber final {
  public:
    explicit JsonScrubber(Json& value) noexcept : value_(value) {}
    ~JsonScrubber() noexcept { erase_json(value_); }

  private:
    Json& value_;
};

[[nodiscard]] minitun_error_code public_error_code(const ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::ok:
        return MINITUN_ERROR_OK;
    case ErrorCode::invalid_argument:
        return MINITUN_ERROR_INVALID_ARGUMENT;
    case ErrorCode::not_found:
        return MINITUN_ERROR_NOT_FOUND;
    case ErrorCode::already_exists:
        return MINITUN_ERROR_ALREADY_EXISTS;
    case ErrorCode::permission_denied:
        return MINITUN_ERROR_PERMISSION_DENIED;
    case ErrorCode::not_authenticated:
        return MINITUN_ERROR_NOT_AUTHENTICATED;
    case ErrorCode::authentication_failed:
        return MINITUN_ERROR_AUTHENTICATION_FAILED;
    case ErrorCode::connection_failed:
        return MINITUN_ERROR_CONNECTION_FAILED;
    case ErrorCode::connection_timeout:
        return MINITUN_ERROR_CONNECTION_TIMEOUT;
    case ErrorCode::remote_port_in_use:
        return MINITUN_ERROR_REMOTE_PORT_IN_USE;
    case ErrorCode::local_connect_failed:
        return MINITUN_ERROR_LOCAL_CONNECT_FAILED;
    case ErrorCode::protocol_error:
    case ErrorCode::frame_too_large:
        return MINITUN_ERROR_PROTOCOL;
    case ErrorCode::unsupported_version:
        return MINITUN_ERROR_UNSUPPORTED_VERSION;
    case ErrorCode::resource_exhausted:
        return MINITUN_ERROR_RESOURCE_EXHAUSTED;
    case ErrorCode::database_error:
        return MINITUN_ERROR_DATABASE;
    case ErrorCode::tls_error:
        return MINITUN_ERROR_TLS;
    case ErrorCode::ipc_error:
        return MINITUN_ERROR_IPC;
    case ErrorCode::internal_error:
        return MINITUN_ERROR_INTERNAL;
    }
    return MINITUN_ERROR_INTERNAL;
}

[[nodiscard]] char* duplicate_text(const std::string_view value) {
    char* output = new char[value.size() + 1U];
    std::memcpy(output, value.data(), value.size());
    output[value.size()] = '\0';
    return output;
}

void publish_error(const Error& source, minitun_error** const output) noexcept {
    if (output == nullptr) {
        return;
    }
    *output = nullptr;
    try {
        auto error = std::make_unique<minitun_error>();
        error->code = public_error_code(source.code());
        error->message = duplicate_text(source.message());
        *output = error.release();
    } catch (...) {
    }
}

[[nodiscard]] int fail(const Error& error, minitun_error** const output) noexcept {
    publish_error(error, output);
    return static_cast<int>(public_error_code(error.code()));
}

[[nodiscard]] int internal_failure(minitun_error** const output) noexcept {
    return fail(Error{ErrorCode::internal_error, "local control SDK operation failed"}, output);
}

void initialize_error(minitun_error** const error) noexcept {
    if (error != nullptr) {
        *error = nullptr;
    }
}

[[nodiscard]] Result<std::string_view> bounded_string(const char* const value,
                                                      const std::string_view description,
                                                      const bool nullable = false) {
    if (value == nullptr) {
        if (nullable) {
            return std::string_view{};
        }
        return Error{ErrorCode::invalid_argument, std::string{description} + " is required"};
    }
    const std::size_t length = ::strnlen(value, kMaximumInputBytes + 1U);
    if (length == 0U || length > kMaximumInputBytes) {
        return Error{ErrorCode::invalid_argument, std::string{description} + " is invalid"};
    }
    return std::string_view{value, length};
}

[[nodiscard]] Result<Json> request(minitun_client* const client, std::string method, Json params) {
    const JsonScrubber scrubber{params};
    if (client == nullptr) {
        return Error{ErrorCode::invalid_argument, "client handle is null"};
    }
    auto request_id = minitun::common::Id::generate(minitun::common::IdKind::request);
    if (!request_id) {
        return request_id.error();
    }
    minitun::ipc::LocalClient local{
        minitun::ipc::LocalClientOptions{.socket_path = client->socket_path}};
    auto response = local.request({minitun::ipc::kProtocolVersion, std::move(*request_id),
                                   std::move(method), std::move(params)});
    if (!response) {
        return response.error();
    }
    if (!response->ok()) {
        return response->error() != nullptr
                   ? Result<Json>::failure(*response->error())
                   : Result<Json>::failure(ErrorCode::protocol_error,
                                           "daemon returned an invalid error response");
    }
    if (response->result() == nullptr || !response->result()->is_object()) {
        return Error{ErrorCode::protocol_error, "daemon returned an invalid result"};
    }
    return *response->result();
}

[[nodiscard]] Result<char*> json_string(const Json& object, const std::string_view field,
                                        const bool nullable = false) {
    const auto value = object.find(field);
    if (value == object.end() || (nullable && value->is_null())) {
        return nullable ? static_cast<char*>(nullptr)
                        : Result<char*>::failure(ErrorCode::protocol_error,
                                                 "daemon result is missing a string field");
    }
    if (!value->is_string()) {
        return Error{ErrorCode::protocol_error, "daemon result contains an invalid string field"};
    }
    return duplicate_text(value->get_ref<const std::string&>());
}

[[nodiscard]] Result<std::uint64_t> json_unsigned(const Json& object,
                                                  const std::string_view field) {
    const auto value = object.find(field);
    if (value == object.end() ||
        (!value->is_number_unsigned() &&
         (!value->is_number_integer() || value->get<std::int64_t>() < 0))) {
        return Error{ErrorCode::protocol_error, "daemon result contains an invalid counter"};
    }
    return value->is_number_unsigned() ? value->get<std::uint64_t>()
                                       : static_cast<std::uint64_t>(value->get<std::int64_t>());
}

[[nodiscard]] bool json_boolean(const Json& object, const std::string_view field, bool& output) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_boolean()) {
        return false;
    }
    output = value->get<bool>();
    return true;
}

[[nodiscard]] Result<void> fill_server_info(const Json& server, minitun_server_info& output) {
    minitun_server_info temporary{};
    auto id = json_string(server, "id");
    auto name = json_string(server, "name", true);
    auto endpoint = json_string(server, "endpoint");
    auto tls_name = json_string(server, "tls_server_name", true);
    auto desired = json_string(server, "desired_state");
    auto actual = json_string(server, "actual_state");
    auto revision = json_unsigned(server, "config_revision");
    bool psk = false;
    bool ca = false;
    bool certificate = false;
    bool managed = false;
    if (!id || !name || !endpoint || !tls_name || !desired || !actual || !revision ||
        !json_boolean(server, "credential_configured", psk) ||
        !json_boolean(server, "ca_configured", ca) ||
        !json_boolean(server, "client_certificate_configured", certificate) ||
        !json_boolean(server, "managed_by_config", managed)) {
        delete[] (id ? *id : nullptr);
        delete[] (name ? *name : nullptr);
        delete[] (endpoint ? *endpoint : nullptr);
        delete[] (tls_name ? *tls_name : nullptr);
        delete[] (desired ? *desired : nullptr);
        delete[] (actual ? *actual : nullptr);
        return Error{ErrorCode::protocol_error, "daemon returned an invalid server record"};
    }
    temporary.id = *id;
    temporary.name = *name;
    temporary.endpoint = *endpoint;
    temporary.tls_server_name = *tls_name;
    temporary.desired_state = *desired;
    temporary.actual_state = *actual;
    temporary.config_revision = *revision;
    temporary.credential_configured = psk ? 1U : 0U;
    temporary.ca_configured = ca ? 1U : 0U;
    temporary.client_certificate_configured = certificate ? 1U : 0U;
    temporary.managed_by_config = managed ? 1U : 0U;
    output = temporary;
    return Result<void>::success();
}

[[nodiscard]] Result<void> fill_tunnel_info(const Json& tunnel, minitun_tunnel_info& output) {
    minitun_tunnel_info temporary{};
    auto id = json_string(tunnel, "id");
    auto name = json_string(tunnel, "name", true);
    auto server_id = json_string(tunnel, "server_id");
    auto local = json_string(tunnel, "local_endpoint");
    auto remote = json_string(tunnel, "remote_endpoint");
    auto desired = json_string(tunnel, "desired_state");
    auto actual = json_string(tunnel, "actual_state");
    auto revision = json_unsigned(tunnel, "config_revision");
    bool managed = false;
    if (!id || !name || !server_id || !local || !remote || !desired || !actual || !revision ||
        !json_boolean(tunnel, "managed_by_config", managed)) {
        delete[] (id ? *id : nullptr);
        delete[] (name ? *name : nullptr);
        delete[] (server_id ? *server_id : nullptr);
        delete[] (local ? *local : nullptr);
        delete[] (remote ? *remote : nullptr);
        delete[] (desired ? *desired : nullptr);
        delete[] (actual ? *actual : nullptr);
        return Error{ErrorCode::protocol_error, "daemon returned an invalid tunnel record"};
    }
    temporary.id = *id;
    temporary.name = *name;
    temporary.server_id = *server_id;
    temporary.local_endpoint = *local;
    temporary.remote_endpoint = *remote;
    temporary.desired_state = *desired;
    temporary.actual_state = *actual;
    temporary.config_revision = *revision;
    temporary.managed_by_config = managed ? 1U : 0U;
    output = temporary;
    return Result<void>::success();
}

[[nodiscard]] Result<minitun::common::SecureString> read_credential_file(const char* path_value,
                                                                         const bool private_file) {
    auto path = bounded_string(path_value, "credential file path");
    if (!path || path->size() > kMaximumPathBytes) {
        return path ? Result<minitun::common::SecureString>::failure(
                          ErrorCode::invalid_argument, "credential file path is too long")
                    : Result<minitun::common::SecureString>::failure(path.error());
    }
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const std::string path_string{*path};
    const int descriptor = ::open(path_string.c_str(), flags);
    if (descriptor < 0) {
        return Error{ErrorCode::permission_denied, "credential file cannot be opened"};
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > kMaximumInputBytes ||
        (status.st_mode & (private_file ? 0077 : 0022)) != 0 || status.st_uid != ::geteuid()) {
        static_cast<void>(::close(descriptor));
        return Error{ErrorCode::permission_denied, "credential file permissions are unsafe"};
    }
    std::string contents(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0U;
    while (offset < contents.size()) {
        const ssize_t count =
            ::read(descriptor, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            static_cast<void>(::close(descriptor));
            minitun::common::secure_erase_memory(contents.data(), contents.size());
            return Error{ErrorCode::invalid_argument, "credential file could not be read"};
        }
        offset += static_cast<std::size_t>(count);
    }
    static_cast<void>(::close(descriptor));
    if (contents.find('\0') != std::string::npos) {
        minitun::common::secure_erase_memory(contents.data(), contents.size());
        return Error{ErrorCode::invalid_argument, "credential file contains a NUL byte"};
    }
    minitun::common::SecureString secret{contents};
    minitun::common::secure_erase_memory(contents.data(), contents.size());
    return secret;
}

[[nodiscard]] Result<void> add_file(Json& params, const std::string_view field, const char* path,
                                    const bool private_file) {
    if (path == nullptr) {
        params[std::string{field}] = nullptr;
        return Result<void>::success();
    }
    auto material = read_credential_file(path, private_file);
    if (!material) {
        return material.error();
    }
    params[std::string{field}] = std::string{material->view()};
    material->clear();
    return Result<void>::success();
}

template <typename Callable> int api_guard(minitun_error** error, Callable&& callable) noexcept {
    initialize_error(error);
    try {
        auto result = std::forward<Callable>(callable)();
        if (!result) {
            return fail(result.error(), error);
        }
        return 0;
    } catch (const std::bad_alloc&) {
        return fail(Error{ErrorCode::resource_exhausted, "local control SDK ran out of memory"},
                    error);
    } catch (...) {
        return internal_failure(error);
    }
}

} // namespace

extern "C" {

uint32_t minitun_client_abi_version(void) { return MINITUN_CLIENT_ABI_VERSION; }

int minitun_client_create(const minitun_client_options* options, minitun_client** output,
                          minitun_error** error) {
    return api_guard(error, [options, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "output client pointer is required"};
        }
        *output = nullptr;
        if (options != nullptr && options->struct_size < sizeof(minitun_client_options)) {
            return Error{ErrorCode::unsupported_version, "client options structure is too small"};
        }
        std::string socket_path{minitun::ipc::kDefaultSocketPath};
        if (options != nullptr && options->socket_path != nullptr) {
            auto path = bounded_string(options->socket_path, "daemon socket path");
            if (!path || path->size() > kMaximumPathBytes) {
                return path ? Result<void>::failure(ErrorCode::invalid_argument,
                                                    "daemon socket path is too long")
                            : Result<void>::failure(path.error());
            }
            socket_path.assign(*path);
        }
        auto created = std::make_unique<minitun_client>();
        created->socket_path = std::move(socket_path);
        *output = created.release();
        return Result<void>::success();
    });
}

void minitun_client_destroy(minitun_client* client) { delete client; }

void minitun_error_free(minitun_error* error) {
    if (error != nullptr) {
        delete[] error->message;
        delete error;
    }
}

int minitun_client_identity_get(minitun_client* client, minitun_identity* output,
                                minitun_error** error) {
    return api_guard(error, [client, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "identity output is required"};
        }
        *output = {};
        auto result = request(client, "daemon.identity", Json::object());
        if (!result) {
            return result.error();
        }
        auto id = json_string(*result, "client_id");
        if (!id) {
            return id.error();
        }
        output->client_id = *id;
        return Result<void>::success();
    });
}

void minitun_identity_free(minitun_identity* value) {
    if (value != nullptr) {
        delete[] value->client_id;
        *value = {};
    }
}

int minitun_client_status_get(minitun_client* client, minitun_status* output,
                              minitun_error** error) {
    return api_guard(error, [client, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "status output is required"};
        }
        *output = {};
        auto result = request(client, "status", Json::object());
        if (!result) {
            return result.error();
        }
        try {
            auto server_total = json_unsigned(result->at("servers"), "total");
            auto server_online = json_unsigned(result->at("servers"), "online");
            auto tunnel_total = json_unsigned(result->at("tunnels"), "total");
            auto tunnel_active = json_unsigned(result->at("tunnels"), "active");
            const auto& runtime = result->at("runtime");
            auto sessions = json_unsigned(runtime.at("sessions"), "active");
            auto workers_idle = json_unsigned(runtime.at("workers"), "idle");
            auto workers_active = json_unsigned(runtime.at("workers"), "active");
            auto connections = json_unsigned(runtime.at("connections"), "active");
            if (!server_total || !server_online || !tunnel_total || !tunnel_active || !sessions ||
                !workers_idle || !workers_active || !connections) {
                return Error{ErrorCode::protocol_error, "daemon returned an invalid status"};
            }
            *output = {*server_total, *server_online, *tunnel_total,   *tunnel_active,
                       *sessions,     *workers_idle,  *workers_active, *connections};
            return Result<void>::success();
        } catch (const Json::exception&) {
            return Error{ErrorCode::protocol_error, "daemon returned an invalid status"};
        }
    });
}

int minitun_client_server_create(minitun_client* client,
                                 const minitun_server_create_request* create,
                                 minitun_server_info* output, minitun_error** error) {
    return api_guard(error, [client, create, output]() -> Result<void> {
        if (create == nullptr || create->struct_size < sizeof(*create) || output == nullptr) {
            return Error{ErrorCode::invalid_argument, "server create request is invalid"};
        }
        *output = {};
        auto endpoint = bounded_string(create->endpoint, "server endpoint");
        if (!endpoint) {
            return endpoint.error();
        }
        Json params{{"endpoint", *endpoint}};
        if (create->name != nullptr) {
            auto name = bounded_string(create->name, "server name");
            if (!name) {
                return name.error();
            }
            params["name"] = *name;
        }
        auto result = request(client, "server.add", std::move(params));
        return result ? fill_server_info(result->at("server"), *output)
                      : Result<void>::failure(result.error());
    });
}

int minitun_client_server_login(minitun_client* client, const char* identifier, const char* psk,
                                minitun_server_info* output, minitun_error** error) {
    return api_guard(error, [client, identifier, psk, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "server output is required"};
        }
        *output = {};
        auto id = bounded_string(identifier, "server identifier");
        auto secret = bounded_string(psk, "server PSK");
        if (!id || !secret) {
            return !id ? Result<void>::failure(id.error()) : Result<void>::failure(secret.error());
        }
        auto result = request(client, "server.login", Json{{"identifier", *id}, {"psk", *secret}});
        return result ? fill_server_info(result->at("server"), *output)
                      : Result<void>::failure(result.error());
    });
}

int minitun_client_server_update(minitun_client* client,
                                 const minitun_server_update_request* update,
                                 minitun_server_info* output, minitun_error** error) {
    return api_guard(error, [client, update, output]() -> Result<void> {
        if (update == nullptr || update->struct_size < sizeof(*update) || output == nullptr) {
            return Error{ErrorCode::invalid_argument, "server update request is invalid"};
        }
        *output = {};
        auto id = bounded_string(update->identifier, "server identifier");
        if (!id) {
            return id.error();
        }
        Json params{{"identifier", *id}};
        const auto add_nullable = [&params, update](const std::uint32_t mask, const char* value,
                                                    const std::string_view field) -> Result<void> {
            if ((update->field_mask & mask) == 0U) {
                return Result<void>::success();
            }
            if (value == nullptr) {
                params[std::string{field}] = nullptr;
                return Result<void>::success();
            }
            auto text = bounded_string(value, field);
            if (!text) {
                return text.error();
            }
            params[std::string{field}] = *text;
            return Result<void>::success();
        };
        for (auto value :
             {add_nullable(MINITUN_SERVER_UPDATE_NAME, update->name, "name"),
              add_nullable(MINITUN_SERVER_UPDATE_ENDPOINT, update->endpoint, "endpoint"),
              add_nullable(MINITUN_SERVER_UPDATE_TLS_SERVER_NAME, update->tls_server_name,
                           "tls_server_name")}) {
            if (!value) {
                return value.error();
            }
        }
        if ((update->field_mask & MINITUN_SERVER_UPDATE_CA_FILE) != 0U) {
            if (auto added = add_file(params, "ca_certificate", update->ca_file, false); !added) {
                return added.error();
            }
        }
        if ((update->field_mask & MINITUN_SERVER_UPDATE_CLIENT_CERT_FILE) != 0U) {
            if (auto added =
                    add_file(params, "client_certificate", update->client_cert_file, false);
                !added) {
                return added.error();
            }
        }
        if ((update->field_mask & MINITUN_SERVER_UPDATE_CLIENT_KEY_FILE) != 0U) {
            if (auto added = add_file(params, "client_private_key", update->client_key_file, true);
                !added) {
                return added.error();
            }
        }
        auto result = request(client, "server.update", std::move(params));
        return result ? fill_server_info(result->at("server"), *output)
                      : Result<void>::failure(result.error());
    });
}

int minitun_client_server_execute(minitun_client* client, const char* identifier,
                                  const minitun_server_action action, minitun_server_info* output,
                                  minitun_error** error) {
    return api_guard(error, [client, identifier, action, output]() -> Result<void> {
        auto id = bounded_string(identifier, "server identifier");
        if (!id) {
            return id.error();
        }
        std::string method;
        switch (action) {
        case MINITUN_SERVER_ENABLE:
            method = "server.enable";
            break;
        case MINITUN_SERVER_DISABLE:
            method = "server.disable";
            break;
        case MINITUN_SERVER_LOGOUT:
            method = "server.logout";
            break;
        case MINITUN_SERVER_REMOVE:
            method = "server.remove";
            break;
        default:
            return Error{ErrorCode::invalid_argument, "server action is invalid"};
        }
        if (output != nullptr) {
            *output = {};
        }
        auto result = request(client, std::move(method), Json{{"identifier", *id}});
        if (!result) {
            return result.error();
        }
        if (action == MINITUN_SERVER_REMOVE || output == nullptr) {
            return Result<void>::success();
        }
        return fill_server_info(result->at("server"), *output);
    });
}

int minitun_client_server_list(minitun_client* client, minitun_server_list* output,
                               minitun_error** error) {
    return api_guard(error, [client, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "server list output is required"};
        }
        *output = {};
        auto result = request(client, "server.list", Json::object());
        if (!result) {
            return result.error();
        }
        const auto servers = result->find("servers");
        if (servers == result->end() || !servers->is_array()) {
            return Error{ErrorCode::protocol_error, "daemon returned an invalid server list"};
        }
        auto items = std::make_unique<minitun_server_info[]>(servers->size());
        for (std::size_t index = 0U; index < servers->size(); ++index) {
            auto filled = fill_server_info((*servers)[index], items[index]);
            if (!filled) {
                minitun_server_list partial{items.release(), index};
                minitun_server_list_free(&partial);
                return filled.error();
            }
        }
        output->items = items.release();
        output->size = servers->size();
        return Result<void>::success();
    });
}

void minitun_server_info_free(minitun_server_info* value) {
    if (value != nullptr) {
        delete[] value->id;
        delete[] value->name;
        delete[] value->endpoint;
        delete[] value->tls_server_name;
        delete[] value->desired_state;
        delete[] value->actual_state;
        *value = {};
    }
}

void minitun_server_list_free(minitun_server_list* value) {
    if (value != nullptr) {
        for (std::size_t index = 0U; index < value->size; ++index) {
            minitun_server_info_free(&value->items[index]);
        }
        delete[] value->items;
        *value = {};
    }
}

int minitun_client_tunnel_create(minitun_client* client,
                                 const minitun_tunnel_create_request* create,
                                 minitun_tunnel_info* output, minitun_error** error) {
    return api_guard(error, [client, create, output]() -> Result<void> {
        if (create == nullptr || create->struct_size < sizeof(*create) || output == nullptr ||
            create->local_port == 0U || create->remote_port == 0U) {
            return Error{ErrorCode::invalid_argument, "tunnel create request is invalid"};
        }
        *output = {};
        auto server = bounded_string(create->server, "server identifier");
        if (!server) {
            return server.error();
        }
        Json params{{"server", *server},
                    {"local_port", create->local_port},
                    {"remote_port", create->remote_port}};
        if (create->name != nullptr) {
            auto name = bounded_string(create->name, "tunnel name");
            if (!name) {
                return name.error();
            }
            params["name"] = *name;
        }
        if (create->local_host != nullptr) {
            auto host = bounded_string(create->local_host, "local host");
            if (!host) {
                return host.error();
            }
            params["local_host"] = *host;
        }
        auto result = request(client, "tun.add", std::move(params));
        return result ? fill_tunnel_info(result->at("tunnel"), *output)
                      : Result<void>::failure(result.error());
    });
}

int minitun_client_tunnel_update(minitun_client* client,
                                 const minitun_tunnel_update_request* update,
                                 minitun_tunnel_info* output, minitun_error** error) {
    return api_guard(error, [client, update, output]() -> Result<void> {
        if (update == nullptr || update->struct_size < sizeof(*update) || output == nullptr) {
            return Error{ErrorCode::invalid_argument, "tunnel update request is invalid"};
        }
        *output = {};
        auto id = bounded_string(update->identifier, "tunnel identifier");
        if (!id) {
            return id.error();
        }
        Json params{{"identifier", *id}};
        if ((update->field_mask & MINITUN_TUNNEL_UPDATE_NAME) != 0U) {
            if (update->name == nullptr) {
                params["name"] = nullptr;
            } else {
                auto value = bounded_string(update->name, "tunnel name");
                if (!value) {
                    return value.error();
                }
                params["name"] = *value;
            }
        }
        if ((update->field_mask & MINITUN_TUNNEL_UPDATE_LOCAL_HOST) != 0U) {
            auto value = bounded_string(update->local_host, "local host");
            if (!value) {
                return value.error();
            }
            params["local_host"] = *value;
        }
        if ((update->field_mask & MINITUN_TUNNEL_UPDATE_LOCAL_PORT) != 0U) {
            if (update->local_port == 0U) {
                return Error{ErrorCode::invalid_argument, "local port is invalid"};
            }
            params["local_port"] = update->local_port;
        }
        if ((update->field_mask & MINITUN_TUNNEL_UPDATE_REMOTE_PORT) != 0U) {
            if (update->remote_port == 0U) {
                return Error{ErrorCode::invalid_argument, "remote port is invalid"};
            }
            params["remote_port"] = update->remote_port;
        }
        auto result = request(client, "tun.update", std::move(params));
        return result ? fill_tunnel_info(result->at("tunnel"), *output)
                      : Result<void>::failure(result.error());
    });
}

int minitun_client_tunnel_execute(minitun_client* client, const char* identifier,
                                  const minitun_tunnel_action action, minitun_tunnel_info* output,
                                  minitun_error** error) {
    return api_guard(error, [client, identifier, action, output]() -> Result<void> {
        auto id = bounded_string(identifier, "tunnel identifier");
        if (!id) {
            return id.error();
        }
        std::string method;
        switch (action) {
        case MINITUN_TUNNEL_ENABLE:
            method = "tun.enable";
            break;
        case MINITUN_TUNNEL_DISABLE:
            method = "tun.disable";
            break;
        case MINITUN_TUNNEL_REMOVE:
            method = "tun.remove";
            break;
        default:
            return Error{ErrorCode::invalid_argument, "tunnel action is invalid"};
        }
        if (output != nullptr) {
            *output = {};
        }
        auto result = request(client, std::move(method), Json{{"identifier", *id}});
        if (!result) {
            return result.error();
        }
        if (action == MINITUN_TUNNEL_REMOVE || output == nullptr) {
            return Result<void>::success();
        }
        return fill_tunnel_info(result->at("tunnel"), *output);
    });
}

int minitun_client_tunnel_list(minitun_client* client, const char* server,
                               minitun_tunnel_list* output, minitun_error** error) {
    return api_guard(error, [client, server, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "tunnel list output is required"};
        }
        *output = {};
        Json params = Json::object();
        if (server != nullptr) {
            auto value = bounded_string(server, "server identifier");
            if (!value) {
                return value.error();
            }
            params["server"] = *value;
        }
        auto result = request(client, "tun.list", std::move(params));
        if (!result) {
            return result.error();
        }
        const auto tunnels = result->find("tunnels");
        if (tunnels == result->end() || !tunnels->is_array()) {
            return Error{ErrorCode::protocol_error, "daemon returned an invalid tunnel list"};
        }
        auto items = std::make_unique<minitun_tunnel_info[]>(tunnels->size());
        for (std::size_t index = 0U; index < tunnels->size(); ++index) {
            auto filled = fill_tunnel_info((*tunnels)[index], items[index]);
            if (!filled) {
                minitun_tunnel_list partial{items.release(), index};
                minitun_tunnel_list_free(&partial);
                return filled.error();
            }
        }
        output->items = items.release();
        output->size = tunnels->size();
        return Result<void>::success();
    });
}

void minitun_tunnel_info_free(minitun_tunnel_info* value) {
    if (value != nullptr) {
        delete[] value->id;
        delete[] value->name;
        delete[] value->server_id;
        delete[] value->local_endpoint;
        delete[] value->remote_endpoint;
        delete[] value->desired_state;
        delete[] value->actual_state;
        *value = {};
    }
}

void minitun_tunnel_list_free(minitun_tunnel_list* value) {
    if (value != nullptr) {
        for (std::size_t index = 0U; index < value->size; ++index) {
            minitun_tunnel_info_free(&value->items[index]);
        }
        delete[] value->items;
        *value = {};
    }
}

static Result<void> fill_config_plan(const Json& result, minitun_config_plan_result& output) {
    const auto actions = result.find("actions");
    const auto prune = result.find("prune");
    if (actions == result.end() || !actions->is_array() || prune == result.end() ||
        !prune->is_boolean()) {
        return Error{ErrorCode::protocol_error, "daemon returned an invalid configuration plan"};
    }
    auto items = std::make_unique<minitun_config_action_info[]>(actions->size());
    for (std::size_t index = 0U; index < actions->size(); ++index) {
        const auto& action = (*actions)[index];
        if (!action.is_object()) {
            minitun_config_plan_result partial{items.release(), index, 0U};
            minitun_config_plan_result_free(&partial);
            return Error{ErrorCode::protocol_error,
                         "daemon returned an invalid configuration action"};
        }
        const auto action_text = action.find("action");
        const auto resource_text = action.find("resource");
        if (action_text == action.end() || !action_text->is_string() ||
            resource_text == action.end() || !resource_text->is_string()) {
            minitun_config_plan_result partial{items.release(), index, 0U};
            minitun_config_plan_result_free(&partial);
            return Error{ErrorCode::protocol_error,
                         "daemon returned an invalid configuration action"};
        }
        const std::string action_value = action_text->get<std::string>();
        const std::string resource_value = resource_text->get<std::string>();
        if (action_value == "create") {
            items[index].action = MINITUN_CONFIG_CREATE;
        } else if (action_value == "update") {
            items[index].action = MINITUN_CONFIG_UPDATE;
        } else if (action_value == "disable") {
            items[index].action = MINITUN_CONFIG_DISABLE;
        } else if (action_value == "delete") {
            items[index].action = MINITUN_CONFIG_DELETE;
        } else {
            minitun_config_plan_result partial{items.release(), index, 0U};
            minitun_config_plan_result_free(&partial);
            return Error{ErrorCode::protocol_error,
                         "daemon returned an unknown configuration action"};
        }
        if (resource_value == "server") {
            items[index].resource = MINITUN_CONFIG_SERVER;
        } else if (resource_value == "tunnel") {
            items[index].resource = MINITUN_CONFIG_TUNNEL;
        } else {
            minitun_config_plan_result partial{items.release(), index, 0U};
            minitun_config_plan_result_free(&partial);
            return Error{ErrorCode::protocol_error,
                         "daemon returned an unknown configuration resource"};
        }
        auto id = json_string(action, "id", true);
        auto name = json_string(action, "name", true);
        if (!id || !name) {
            minitun_config_plan_result partial{items.release(), index + 1U, 0U};
            minitun_config_plan_result_free(&partial);
            return Error{ErrorCode::protocol_error,
                         "daemon returned an invalid configuration action identity"};
        }
        items[index].id = *id;
        items[index].name = *name;
    }
    output.actions = items.release();
    output.size = actions->size();
    output.prune = prune->get<bool>() ? 1U : 0U;
    return Result<void>::success();
}

int minitun_client_config_plan(minitun_client* client, const char* path, const uint8_t prune,
                               minitun_config_plan_result* output, minitun_error** error) {
    return api_guard(error, [client, path, prune, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "configuration plan output is required"};
        }
        *output = {};
        auto value = bounded_string(path, "configuration path");
        if (!value) {
            return value.error();
        }
        auto result =
            request(client, "config.plan", Json{{"path", *value}, {"prune", prune != 0U}});
        return result ? fill_config_plan(*result, *output) : Result<void>::failure(result.error());
    });
}

int minitun_client_config_apply(minitun_client* client, const char* path, const uint8_t prune,
                                minitun_config_plan_result* output, minitun_error** error) {
    return api_guard(error, [client, path, prune, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "configuration apply output is required"};
        }
        *output = {};
        auto value = bounded_string(path, "configuration path");
        if (!value) {
            return value.error();
        }
        auto result =
            request(client, "config.apply", Json{{"path", *value}, {"prune", prune != 0U}});
        return result ? fill_config_plan(*result, *output) : Result<void>::failure(result.error());
    });
}

void minitun_config_plan_result_free(minitun_config_plan_result* value) {
    if (value != nullptr) {
        for (std::size_t index = 0U; index < value->size; ++index) {
            delete[] value->actions[index].id;
            delete[] value->actions[index].name;
        }
        delete[] value->actions;
        *value = {};
    }
}

int minitun_client_config_export(minitun_client* client, minitun_config_snapshot* output,
                                 minitun_error** error) {
    return api_guard(error, [client, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "configuration snapshot output is required"};
        }
        *output = {};
        minitun_error* nested_error = nullptr;
        const int servers = minitun_client_server_list(client, &output->servers, &nested_error);
        if (servers != 0) {
            Error converted{ErrorCode::internal_error,
                            nested_error != nullptr && nested_error->message != nullptr
                                ? nested_error->message
                                : "server snapshot failed"};
            minitun_error_free(nested_error);
            return converted;
        }
        const int tunnels =
            minitun_client_tunnel_list(client, nullptr, &output->tunnels, &nested_error);
        if (tunnels != 0) {
            minitun_server_list_free(&output->servers);
            Error converted{ErrorCode::internal_error,
                            nested_error != nullptr && nested_error->message != nullptr
                                ? nested_error->message
                                : "tunnel snapshot failed"};
            minitun_error_free(nested_error);
            return converted;
        }
        return Result<void>::success();
    });
}

void minitun_config_snapshot_free(minitun_config_snapshot* value) {
    if (value != nullptr) {
        minitun_server_list_free(&value->servers);
        minitun_tunnel_list_free(&value->tunnels);
        *value = {};
    }
}

int minitun_client_diagnostics_get(minitun_client* client, minitun_diagnostics* output,
                                   minitun_error** error) {
    return api_guard(error, [client, output]() -> Result<void> {
        if (output == nullptr) {
            return Error{ErrorCode::invalid_argument, "diagnostics output is required"};
        }
        *output = {};
        auto health = request(client, "health", Json::object());
        auto readiness = request(client, "readiness", Json::object());
        if (!health) {
            return health.error();
        }
        if (!readiness) {
            return readiness.error();
        }
        bool state = false;
        bool credentials = false;
        bool ready = false;
        const auto status = health->find("status");
        if (status == health->end() || !status->is_string() ||
            !json_boolean(*health, "state_db", state) ||
            !json_boolean(*health, "credentials_db", credentials) ||
            !json_boolean(*readiness, "ready", ready)) {
            return Error{ErrorCode::protocol_error, "daemon returned invalid diagnostics"};
        }
        output->healthy = status->get_ref<const std::string&>() == "ok" ? 1U : 0U;
        output->ready = ready ? 1U : 0U;
        output->state_database_ok = state ? 1U : 0U;
        output->credential_database_ok = credentials ? 1U : 0U;
        return Result<void>::success();
    });
}

} // extern "C"
