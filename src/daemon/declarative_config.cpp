#include <minitun/daemon/declarative_config.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <minitun/common/endpoint.hpp>
#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>
#include <minitun/daemon/credential_keys.hpp>
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

inline constexpr std::size_t kMaximumPathBytes = 4'096U;
inline constexpr std::size_t kMaximumConfigBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumJsonDepth = 12U;
inline constexpr std::size_t kMaximumJsonNodes = 100'000U;
inline constexpr std::size_t kMaximumJsonStringBytes = 64U * 1024U;

class FileDescriptor final {
  public:
    explicit FileDescriptor(const int descriptor) noexcept : descriptor_(descriptor) {}
    ~FileDescriptor() noexcept {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return descriptor_; }

  private:
    int descriptor_;
};

enum class FileKind : std::uint8_t {
    config,
    public_credential,
    private_credential,
};

[[nodiscard]] Result<std::vector<char>> read_file(const std::string& path, const FileKind kind) {
    if (path.empty() || path.size() > kMaximumPathBytes || path.find('\0') != std::string::npos) {
        return Error{ErrorCode::invalid_argument, "configuration file path is invalid"};
    }
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int raw_descriptor = ::open(path.c_str(), flags);
    if (raw_descriptor < 0) {
        return Error{errno == EACCES ? ErrorCode::permission_denied : ErrorCode::invalid_argument,
                     "configuration input file cannot be opened"};
    }
    const FileDescriptor descriptor{raw_descriptor};
    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1 || status.st_size <= 0) {
        return Error{ErrorCode::permission_denied,
                     "configuration input must be one non-empty regular file"};
    }
    const std::size_t maximum =
        kind == FileKind::config ? kMaximumConfigBytes : storage::kMaxCredentialSecretBytes;
    if (static_cast<std::uint64_t>(status.st_size) > maximum) {
        return Error{ErrorCode::resource_exhausted, "configuration input file exceeds its limit"};
    }
    const mode_t forbidden = kind == FileKind::private_credential ? 0077 : 0022;
    if (status.st_uid != ::geteuid() || (status.st_mode & forbidden) != 0) {
        return Error{ErrorCode::permission_denied,
                     kind == FileKind::private_credential
                         ? "private credential file must be owned by the daemon user and inaccessible to others"
                         : "configuration input must be owned by the daemon user and not writable by others"};
    }
    std::vector<char> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const ssize_t count =
            ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (kind != FileKind::config) {
                common::secure_erase_memory(bytes.data(), bytes.size());
            }
            return Error{ErrorCode::invalid_argument,
                         "configuration input file could not be read completely"};
        }
        offset += static_cast<std::size_t>(count);
    }
    if (std::find(bytes.begin(), bytes.end(), '\0') != bytes.end()) {
        if (kind != FileKind::config) {
            common::secure_erase_memory(bytes.data(), bytes.size());
        }
        return Error{ErrorCode::invalid_argument, "configuration input contains a NUL byte"};
    }
    return bytes;
}

enum class ParseFailure : std::uint8_t {
    none,
    duplicate_key,
    depth,
    nodes,
    string,
};

struct ParseContext final {
    bool object{false};
    std::set<std::string, std::less<>> keys;
};

struct ParseLimits final {
    ParseFailure failure{ParseFailure::none};
    std::size_t nodes{0U};
    std::vector<ParseContext> contexts;

    [[nodiscard]] bool accept(const Json::parse_event_t event, Json& parsed) {
        if (failure != ParseFailure::none) {
            return false;
        }
        if (event == Json::parse_event_t::object_start ||
            event == Json::parse_event_t::array_start) {
            if (contexts.size() >= kMaximumJsonDepth) {
                failure = ParseFailure::depth;
                return false;
            }
            if (++nodes > kMaximumJsonNodes) {
                failure = ParseFailure::nodes;
                return false;
            }
            contexts.push_back({event == Json::parse_event_t::object_start, {}});
            return true;
        }
        if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) {
            if (!contexts.empty()) {
                contexts.pop_back();
            }
            return true;
        }
        if (event == Json::parse_event_t::key) {
            if (++nodes > kMaximumJsonNodes) {
                failure = ParseFailure::nodes;
                return false;
            }
            const auto& key = parsed.get_ref<const std::string&>();
            if (key.size() > kMaximumJsonStringBytes) {
                failure = ParseFailure::string;
                return false;
            }
            if (contexts.empty() || !contexts.back().object ||
                !contexts.back().keys.emplace(key).second) {
                failure = ParseFailure::duplicate_key;
                return false;
            }
            return true;
        }
        if (event == Json::parse_event_t::value) {
            if (++nodes > kMaximumJsonNodes) {
                failure = ParseFailure::nodes;
                return false;
            }
            if (parsed.is_string() &&
                parsed.get_ref<const std::string&>().size() > kMaximumJsonStringBytes) {
                failure = ParseFailure::string;
                return false;
            }
        }
        return true;
    }
};

[[nodiscard]] Result<Json> parse_json(const std::vector<char>& bytes) {
    if (bytes.size() >= 3U && static_cast<unsigned char>(bytes[0]) == 0xefU &&
        static_cast<unsigned char>(bytes[1]) == 0xbbU &&
        static_cast<unsigned char>(bytes[2]) == 0xbfU) {
        return Error{ErrorCode::invalid_argument,
                     "declarative configuration must not contain a UTF-8 BOM"};
    }
    ParseLimits limits;
    const auto callback = [&limits](const int, const Json::parse_event_t event, Json& parsed) {
        return limits.accept(event, parsed);
    };
    try {
        auto document = Json::parse(bytes.cbegin(), bytes.cend(), callback, true, false);
        if (limits.failure == ParseFailure::duplicate_key) {
            return Error{ErrorCode::invalid_argument,
                         "declarative configuration contains a duplicate key"};
        }
        if (limits.failure != ParseFailure::none) {
            return Error{ErrorCode::resource_exhausted,
                         "declarative configuration exceeds a parsing limit"};
        }
        return document;
    } catch (const Json::exception&) {
        return Error{ErrorCode::invalid_argument,
                     "declarative configuration is not strict JSON"};
    } catch (...) {
        return Error{ErrorCode::resource_exhausted,
                     "insufficient memory while parsing declarative configuration"};
    }
}

[[nodiscard]] bool exact_fields(const Json& value,
                                const std::set<std::string_view>& allowed) {
    if (!value.is_object()) {
        return false;
    }
    for (auto field = value.cbegin(); field != value.cend(); ++field) {
        if (!allowed.contains(field.key())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<std::optional<std::string>> nullable_string(const Json& object,
                                                                 const std::string_view field) {
    const auto value = object.find(field);
    if (value == object.end() || value->is_null()) {
        return std::optional<std::string>{};
    }
    if (!value->is_string()) {
        return Error{ErrorCode::invalid_argument,
                     std::string{field} + " must be a string or null"};
    }
    const auto text = value->get<std::string>();
    if (text.empty() || text.size() > kMaximumJsonStringBytes ||
        text.find('\0') != std::string::npos) {
        return Error{ErrorCode::invalid_argument, std::string{field} + " is invalid"};
    }
    return std::optional<std::string>{text};
}

[[nodiscard]] Result<std::string> required_string(const Json& object,
                                                  const std::string_view field) {
    auto value = nullable_string(object, field);
    if (!value) {
        return value.error();
    }
    if (!value->has_value()) {
        return Error{ErrorCode::invalid_argument, std::string{field} + " is required"};
    }
    return std::move(**value);
}

[[nodiscard]] Result<bool> optional_enabled(const Json& object) {
    const auto value = object.find("enabled");
    if (value == object.end()) {
        return true;
    }
    if (!value->is_boolean()) {
        return Error{ErrorCode::invalid_argument, "enabled must be a boolean"};
    }
    return value->get<bool>();
}

[[nodiscard]] Result<std::uint16_t> required_port(const Json& object,
                                                  const std::string_view field) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_number_unsigned()) {
        return Error{ErrorCode::invalid_argument,
                     std::string{field} + " must be an unsigned integer"};
    }
    const auto port = value->get<std::uint64_t>();
    if (port == 0U || port > 65'535U) {
        return Error{ErrorCode::invalid_argument,
                     std::string{field} + " must be between 1 and 65535"};
    }
    return static_cast<std::uint16_t>(port);
}

[[nodiscard]] std::string endpoint_text(const std::string_view host, const std::uint16_t port) {
    if (host.find(':') != std::string_view::npos) {
        return "[" + std::string{host} + "]:" + std::to_string(port);
    }
    return std::string{host} + ":" + std::to_string(port);
}

[[nodiscard]] std::int64_t next_update_time(const std::int64_t previous) noexcept {
    return std::max(previous, common::unix_milliseconds_now());
}

enum class MutationKind : std::uint8_t {
    none,
    create,
    update,
    disable,
    remove,
};

[[nodiscard]] std::string_view mutation_text(const MutationKind kind) noexcept {
    switch (kind) {
    case MutationKind::none:
        return "none";
    case MutationKind::create:
        return "create";
    case MutationKind::update:
        return "update";
    case MutationKind::disable:
        return "disable";
    case MutationKind::remove:
        return "delete";
    }
    return "none";
}

struct CredentialMaterial final {
    ServerCredentialKind kind{ServerCredentialKind::psk};
    std::optional<std::string> previous_reference;
    common::SecureString final_value;
    bool changed{false};
};

struct PreparedServer final {
    std::optional<ServerRecord> original;
    ServerRecord desired;
    std::optional<std::string> action_id;
    std::array<CredentialMaterial, 4> credentials;
    MutationKind mutation{MutationKind::none};
    std::vector<std::string> changes;
    bool transport_changed{false};
    bool desired_state_changed{false};
};

struct PreparedTunnel final {
    std::optional<TunnelRecord> original;
    TunnelRecord desired;
    std::optional<std::string> action_id;
    MutationKind mutation{MutationKind::none};
    std::vector<std::string> changes;
    bool endpoint_changed{false};
    bool desired_state_changed{false};
};

struct PreparedPlan final {
    std::vector<PreparedServer> servers;
    std::vector<PreparedTunnel> tunnels;
    Json actions = Json::array();
};

[[nodiscard]] std::optional<std::string_view>
credential_path_field(const ServerCredentialKind kind) noexcept {
    switch (kind) {
    case ServerCredentialKind::psk:
        return "psk_file";
    case ServerCredentialKind::ca_certificate:
        return "ca_file";
    case ServerCredentialKind::client_certificate:
        return "client_cert_file";
    case ServerCredentialKind::client_private_key:
        return "client_key_file";
    }
    return std::nullopt;
}

[[nodiscard]] FileKind credential_file_kind(const ServerCredentialKind kind) noexcept {
    return kind == ServerCredentialKind::psk || kind == ServerCredentialKind::client_private_key
               ? FileKind::private_credential
               : FileKind::public_credential;
}

[[nodiscard]] Result<common::SecureString>
load_current_material(storage::CredentialStore& credentials,
                      const std::optional<std::string>& reference) {
    if (!reference.has_value()) {
        return common::SecureString{};
    }
    auto loaded = credentials.get(*reference);
    if (!loaded) {
        return Error{loaded.error().code(), "a referenced credential is unavailable"};
    }
    return loaded;
}

[[nodiscard]] Result<CredentialMaterial>
prepare_credential(const Json& server, const std::filesystem::path& base_directory,
                   storage::CredentialStore& credentials, const ServerRecord& current,
                   const ServerCredentialKind kind) {
    CredentialMaterial result;
    result.kind = kind;
    result.previous_reference = credential_reference(current, kind);
    auto current_value = load_current_material(credentials, result.previous_reference);
    if (!current_value) {
        return current_value.error();
    }
    const std::string_view field = *credential_path_field(kind);
    const auto configured = server.find(field);
    if (configured == server.end()) {
        result.final_value = std::move(*current_value);
        return result;
    }
    if (configured->is_null()) {
        result.changed = !current_value->empty();
        return result;
    }
    if (!configured->is_string()) {
        return Error{ErrorCode::invalid_argument,
                     std::string{field} + " must be a file path or null"};
    }
    std::filesystem::path credential_path{configured->get<std::string>()};
    if (credential_path.is_relative()) {
        credential_path = base_directory / credential_path;
    }
    auto bytes = read_file(credential_path.lexically_normal().string(), credential_file_kind(kind));
    if (!bytes) {
        return bytes.error();
    }
    if (kind == ServerCredentialKind::psk) {
        while (!bytes->empty() && (bytes->back() == '\n' || bytes->back() == '\r')) {
            bytes->pop_back();
        }
        if (bytes->empty()) {
            return Error{ErrorCode::invalid_argument,
                         "psk_file contains an empty secret after line-ending normalization"};
        }
    }
    try {
        common::SecureString replacement{std::string_view{bytes->data(), bytes->size()}};
        common::secure_erase_memory(bytes->data(), bytes->size());
        result.changed = !replacement.equals(*current_value);
        result.final_value = std::move(replacement);
        return result;
    } catch (...) {
        common::secure_erase_memory(bytes->data(), bytes->size());
        return Error{ErrorCode::resource_exhausted,
                     "insufficient memory while loading declarative credentials"};
    }
}

void add_change(std::vector<std::string>& changes, const bool changed,
                const std::string_view field) {
    if (changed) {
        changes.emplace_back(field);
    }
}

[[nodiscard]] Json action_json(const MutationKind mutation, const std::string_view resource,
                               const std::optional<std::string>& id,
                               const std::optional<std::string>& name,
                               std::vector<std::string> changes = {}) {
    std::sort(changes.begin(), changes.end());
    return Json{{"action", mutation_text(mutation)},
                {"resource", resource},
                {"id", id.has_value() ? Json(*id) : Json(nullptr)},
                {"name", name.has_value() ? Json(*name) : Json(nullptr)},
                {"changes", std::move(changes)}};
}

[[nodiscard]] bool marker_value(const Json& credentials, const std::string_view field,
                                const bool expected, Error& error) {
    const auto value = credentials.find(field);
    if (value == credentials.end()) {
        return true;
    }
    if (!value->is_boolean() || value->get<bool>() != expected) {
        error = Error{ErrorCode::invalid_argument,
                      "credential availability marker does not match resulting state"};
        return false;
    }
    return true;
}

[[nodiscard]] Result<void> validate_credential_markers(const Json& server,
                                                       const PreparedServer& prepared) {
    const auto markers = server.find("credentials");
    if (markers == server.end()) {
        return Result<void>::success();
    }
    if (!exact_fields(*markers, {"psk", "ca", "client_certificate"})) {
        return Error{ErrorCode::invalid_argument, "credentials markers contain an unknown field"};
    }
    Error error{ErrorCode::ok};
    const bool certificate = !prepared.credentials[2].final_value.empty() &&
                             !prepared.credentials[3].final_value.empty();
    if (!marker_value(*markers, "psk", !prepared.credentials[0].final_value.empty(), error) ||
        !marker_value(*markers, "ca", !prepared.credentials[1].final_value.empty(), error) ||
        !marker_value(*markers, "client_certificate", certificate, error)) {
        return error;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<PreparedPlan>
prepare_plan(storage::StateRepository& repository, storage::CredentialStore& credentials,
             const std::string_view config_path, const bool prune) {
    if (config_path.empty() || config_path.size() > kMaximumPathBytes ||
        config_path.find('\0') != std::string_view::npos) {
        return Error{ErrorCode::invalid_argument, "declarative configuration path is invalid"};
    }
    std::filesystem::path path{config_path};
    auto bytes = read_file(path.lexically_normal().string(), FileKind::config);
    if (!bytes) {
        return bytes.error();
    }
    auto document = parse_json(*bytes);
    if (!document) {
        return document.error();
    }
    if (!exact_fields(*document, {"format_version", "servers", "tunnels"}) ||
        !document->contains("format_version") || !document->at("format_version").is_number_unsigned() ||
        document->at("format_version").get<std::uint64_t>() != 1U ||
        !document->contains("servers") || !document->at("servers").is_array() ||
        !document->contains("tunnels") || !document->at("tunnels").is_array()) {
        return Error{ErrorCode::invalid_argument,
                     "declarative configuration must use strict format_version 1"};
    }

    auto current_servers = repository.servers().list();
    auto current_tunnels = repository.tunnels().list();
    if (!current_servers) {
        return current_servers.error();
    }
    if (!current_tunnels) {
        return current_tunnels.error();
    }
    current_servers->erase(
        std::remove_if(current_servers->begin(), current_servers->end(), [](const auto& server) {
            return server.desired_state == ServerDesiredState::removed;
        }),
        current_servers->end());
    current_tunnels->erase(
        std::remove_if(current_tunnels->begin(), current_tunnels->end(), [](const auto& tunnel) {
            return tunnel.desired_state == TunnelDesiredState::removed;
        }),
        current_tunnels->end());

    PreparedPlan prepared;
    prepared.servers.reserve(document->at("servers").size() + current_servers->size());
    prepared.tunnels.reserve(document->at("tunnels").size() + current_tunnels->size());
    std::unordered_set<std::string> matched_server_ids;
    std::unordered_set<std::string> desired_server_ids;
    std::unordered_set<std::string> desired_server_names;
    const auto base_directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};

    for (const auto& entry : document->at("servers")) {
        if (!exact_fields(entry, {"id", "name", "endpoint", "tls_server_name", "enabled",
                                  "psk_file", "ca_file", "client_cert_file", "client_key_file",
                                  "credentials"})) {
            return Error{ErrorCode::invalid_argument,
                         "a declarative server contains an unknown field"};
        }
        auto id_text = nullable_string(entry, "id");
        auto name = nullable_string(entry, "name");
        auto endpoint_value = required_string(entry, "endpoint");
        auto tls_server_name = nullable_string(entry, "tls_server_name");
        auto enabled = optional_enabled(entry);
        if (!id_text || !name || !endpoint_value || !tls_server_name || !enabled) {
            return !id_text          ? id_text.error()
                   : !name           ? name.error()
                   : !endpoint_value ? endpoint_value.error()
                   : !tls_server_name ? tls_server_name.error()
                                     : enabled.error();
        }
        if (!id_text->has_value() && !name->has_value()) {
            return Error{ErrorCode::invalid_argument,
                         "each declarative server requires an id or name"};
        }
        auto endpoint = common::Endpoint::parse(*endpoint_value);
        if (!endpoint) {
            return endpoint.error();
        }

        std::optional<common::Id> specified_id;
        if (id_text->has_value()) {
            auto parsed = common::Id::parse(**id_text, common::IdKind::server);
            if (!parsed) {
                return parsed.error();
            }
            specified_id = std::move(*parsed);
        }
        const ServerRecord* matched = nullptr;
        if (specified_id.has_value()) {
            const auto found = std::find_if(current_servers->begin(), current_servers->end(),
                                            [&specified_id](const auto& server) {
                                                return server.id == *specified_id;
                                            });
            if (found != current_servers->end()) {
                matched = &*found;
            }
        } else {
            const auto found = std::find_if(current_servers->begin(), current_servers->end(),
                                            [&name](const auto& server) {
                                                return server.name == *name;
                                            });
            if (found != current_servers->end()) {
                matched = &*found;
            }
        }
        std::optional<common::Id> generated_id;
        if (!specified_id.has_value() && matched == nullptr) {
            auto generated = common::Id::generate(common::IdKind::server);
            if (!generated) {
                return generated.error();
            }
            generated_id = std::move(*generated);
        }
        common::Id desired_id = specified_id.has_value()
                                    ? *specified_id
                                    : (matched != nullptr ? matched->id : *generated_id);
        if (!desired_server_ids.emplace(desired_id.str()).second ||
            (name->has_value() && !desired_server_names.emplace(**name).second) ||
            (matched != nullptr && !matched_server_ids.emplace(matched->id.str()).second)) {
            return Error{ErrorCode::already_exists,
                         "declarative servers contain a duplicate id or name"};
        }

        const std::int64_t now = common::unix_milliseconds_now();
        ServerRecord desired = matched != nullptr
                                   ? *matched
                                   : ServerRecord{.id = desired_id,
                                                  .name = std::nullopt,
                                                  .endpoint = *endpoint,
                                                  .credential_ref = std::nullopt,
                                                  .remote_server_id = std::nullopt,
                                                  .desired_state = ServerDesiredState::enabled,
                                                  .actual_state = ServerActualState::not_authenticated,
                                                  .last_error_code = std::nullopt,
                                                  .last_error_message = std::nullopt,
                                                  .reconnect_attempt = 0U,
                                                  .latency_ms = std::nullopt,
                                                  .created_at_unix_ms = now,
                                                  .updated_at_unix_ms = now,
                                                  .tls_server_name = std::nullopt,
                                                  .ca_credential_ref = std::nullopt,
                                                  .client_certificate_ref = std::nullopt,
                                                  .client_private_key_ref = std::nullopt,
                                                  .config_revision = 1U,
                                                  .managed_by_config = false};
        desired.name = *name;
        desired.endpoint = *endpoint;
        desired.tls_server_name = *tls_server_name;
        desired.desired_state = *enabled ? ServerDesiredState::enabled
                                         : ServerDesiredState::disabled;
        desired.managed_by_config = true;

        PreparedServer item{.original = matched != nullptr ? std::optional<ServerRecord>{*matched}
                                                            : std::nullopt,
                            .desired = std::move(desired),
                            .action_id = specified_id.has_value() || matched != nullptr
                                             ? std::optional<std::string>{desired_id.str()}
                                             : std::nullopt,
                            .credentials = {},
                            .mutation = MutationKind::none,
                            .changes = {},
                            .transport_changed = false,
                            .desired_state_changed = false};
        constexpr std::array kinds{ServerCredentialKind::psk,
                                   ServerCredentialKind::ca_certificate,
                                   ServerCredentialKind::client_certificate,
                                   ServerCredentialKind::client_private_key};
        for (std::size_t index = 0U; index < kinds.size(); ++index) {
            auto material = prepare_credential(entry, base_directory, credentials,
                                               matched != nullptr ? *matched : item.desired,
                                               kinds[index]);
            if (!material) {
                return material.error();
            }
            item.credentials[index] = std::move(*material);
        }
        if (item.credentials[2].final_value.empty() != item.credentials[3].final_value.empty()) {
            return Error{ErrorCode::invalid_argument,
                         "client_cert_file and client_key_file must resolve together"};
        }
        auto tls = protocol::make_client_tls_context({
            .ca_certificate_path = {},
            .ca_certificate_pem = item.credentials[1].final_value.view(),
            .client_certificate_pem = item.credentials[2].final_value.view(),
            .client_private_key_pem = item.credentials[3].final_value.view(),
        });
        if (!tls) {
            return tls.error();
        }
        auto markers = validate_credential_markers(entry, item);
        if (!markers) {
            return markers.error();
        }

        for (const auto& material : item.credentials) {
            item.transport_changed = item.transport_changed || material.changed;
            add_change(item.changes, material.changed,
                       *credential_path_field(material.kind));
        }
        if (matched == nullptr) {
            item.mutation = MutationKind::create;
            item.desired.actual_state = item.desired.desired_state == ServerDesiredState::disabled
                                            ? ServerActualState::disabled
                                            : (item.credentials[0].final_value.empty()
                                                   ? ServerActualState::not_authenticated
                                                   : ServerActualState::disconnected);
        } else {
            add_change(item.changes, matched->name != item.desired.name, "name");
            add_change(item.changes, matched->endpoint != item.desired.endpoint, "endpoint");
            add_change(item.changes, matched->tls_server_name != item.desired.tls_server_name,
                       "tls_server_name");
            add_change(item.changes, !matched->managed_by_config, "ownership");
            item.transport_changed = item.transport_changed ||
                                     matched->endpoint != item.desired.endpoint ||
                                     matched->tls_server_name != item.desired.tls_server_name;
            item.desired_state_changed = matched->desired_state != item.desired.desired_state;
            add_change(item.changes, item.desired_state_changed, "enabled");
            if (!item.changes.empty()) {
                item.mutation = item.desired.desired_state == ServerDesiredState::disabled &&
                                        matched->desired_state != ServerDesiredState::disabled
                                    ? MutationKind::disable
                                    : MutationKind::update;
            }
        }
        prepared.servers.push_back(std::move(item));
    }

    std::unordered_map<std::string, const PreparedServer*> desired_servers_by_id;
    std::unordered_map<std::string, const PreparedServer*> desired_servers_by_name;
    for (const auto& server : prepared.servers) {
        desired_servers_by_id.emplace(server.desired.id.str(), &server);
        if (server.desired.name.has_value()) {
            desired_servers_by_name.emplace(*server.desired.name, &server);
        }
    }

    std::unordered_set<std::string> matched_tunnel_ids;
    std::unordered_set<std::string> desired_tunnel_ids;
    std::unordered_set<std::string> desired_tunnel_names;
    for (const auto& entry : document->at("tunnels")) {
        if (!exact_fields(entry, {"id", "name", "server", "local_host", "local_port",
                                  "remote_port", "enabled"})) {
            return Error{ErrorCode::invalid_argument,
                         "a declarative tunnel contains an unknown field"};
        }
        auto id_text = nullable_string(entry, "id");
        auto name = nullable_string(entry, "name");
        auto server_reference = required_string(entry, "server");
        auto local_host = nullable_string(entry, "local_host");
        auto local_port = required_port(entry, "local_port");
        auto remote_port = required_port(entry, "remote_port");
        auto enabled = optional_enabled(entry);
        if (!id_text || !name || !server_reference || !local_host || !local_port || !remote_port ||
            !enabled) {
            return !id_text          ? id_text.error()
                   : !name           ? name.error()
                   : !server_reference ? server_reference.error()
                   : !local_host     ? local_host.error()
                   : !local_port     ? local_port.error()
                   : !remote_port    ? remote_port.error()
                                     : enabled.error();
        }
        if (!id_text->has_value() && !name->has_value()) {
            return Error{ErrorCode::invalid_argument,
                         "each declarative tunnel requires an id or name"};
        }
        const PreparedServer* parent = nullptr;
        auto parsed_server = common::Id::parse(*server_reference, common::IdKind::server);
        if (parsed_server) {
            const auto found = desired_servers_by_id.find(parsed_server->str());
            if (found != desired_servers_by_id.end()) {
                parent = found->second;
            }
        } else {
            const auto found = desired_servers_by_name.find(*server_reference);
            if (found != desired_servers_by_name.end()) {
                parent = found->second;
            }
        }
        if (parent == nullptr) {
            return Error{ErrorCode::not_found,
                         "a declarative tunnel references a server absent from the configuration"};
        }

        std::optional<common::Id> specified_id;
        if (id_text->has_value()) {
            auto parsed = common::Id::parse(**id_text, common::IdKind::tunnel);
            if (!parsed) {
                return parsed.error();
            }
            specified_id = std::move(*parsed);
        }
        const TunnelRecord* matched = nullptr;
        if (specified_id.has_value()) {
            const auto found = std::find_if(current_tunnels->begin(), current_tunnels->end(),
                                            [&specified_id](const auto& tunnel) {
                                                return tunnel.id == *specified_id;
                                            });
            if (found != current_tunnels->end()) {
                matched = &*found;
            }
        } else {
            const auto matches = std::count_if(current_tunnels->begin(), current_tunnels->end(),
                                               [&name](const auto& tunnel) {
                                                   return tunnel.name == *name;
                                               });
            if (matches > 1) {
                return Error{ErrorCode::invalid_argument,
                             "a declarative tunnel name matches multiple existing tunnels"};
            }
            const auto found = std::find_if(current_tunnels->begin(), current_tunnels->end(),
                                            [&name](const auto& tunnel) {
                                                return tunnel.name == *name;
                                            });
            if (found != current_tunnels->end()) {
                matched = &*found;
            }
        }
        std::optional<common::Id> generated_id;
        if (!specified_id.has_value() && matched == nullptr) {
            auto generated = common::Id::generate(common::IdKind::tunnel);
            if (!generated) {
                return generated.error();
            }
            generated_id = std::move(*generated);
        }
        common::Id desired_id = specified_id.has_value()
                                    ? *specified_id
                                    : (matched != nullptr ? matched->id : *generated_id);
        if (!desired_tunnel_ids.emplace(desired_id.str()).second ||
            (name->has_value() && !desired_tunnel_names.emplace(**name).second) ||
            (matched != nullptr && !matched_tunnel_ids.emplace(matched->id.str()).second)) {
            return Error{ErrorCode::already_exists,
                         "declarative tunnels contain a duplicate id or name"};
        }
        if (matched != nullptr && matched->server_id != parent->desired.id) {
            return Error{ErrorCode::invalid_argument,
                         "a tunnel's server ownership and stable id cannot be changed"};
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
        const std::int64_t now = common::unix_milliseconds_now();
        TunnelRecord desired = matched != nullptr
                                   ? *matched
                                   : TunnelRecord{.id = desired_id,
                                                  .name = std::nullopt,
                                                  .server_id = parent->desired.id,
                                                  .protocol = storage::TunnelProtocol::tcp,
                                                  .local_endpoint = *local_endpoint,
                                                  .remote_endpoint = *remote_endpoint,
                                                  .desired_state = TunnelDesiredState::active,
                                                  .actual_state = TunnelActualState::pending,
                                                  .last_error_code = std::nullopt,
                                                  .last_error_message = std::nullopt,
                                                  .created_at_unix_ms = now,
                                                  .updated_at_unix_ms = now,
                                                  .last_synced_at_unix_ms = std::nullopt,
                                                  .config_revision = 1U,
                                                  .managed_by_config = false};
        desired.name = *name;
        desired.local_endpoint = *local_endpoint;
        desired.remote_endpoint = *remote_endpoint;
        desired.desired_state = *enabled ? TunnelDesiredState::active
                                         : TunnelDesiredState::disabled;
        desired.managed_by_config = true;
        PreparedTunnel item{.original = matched != nullptr ? std::optional<TunnelRecord>{*matched}
                                                            : std::nullopt,
                            .desired = std::move(desired),
                            .action_id = specified_id.has_value() || matched != nullptr
                                             ? std::optional<std::string>{desired_id.str()}
                                             : std::nullopt,
                            .mutation = MutationKind::none,
                            .changes = {},
                            .endpoint_changed = false,
                            .desired_state_changed = false};
        if (matched == nullptr) {
            item.mutation = MutationKind::create;
            item.desired.actual_state = item.desired.desired_state == TunnelDesiredState::active
                                            ? TunnelActualState::pending
                                            : TunnelActualState::disabled;
        } else {
            add_change(item.changes, matched->name != item.desired.name, "name");
            add_change(item.changes, matched->local_endpoint != item.desired.local_endpoint,
                       "local_endpoint");
            add_change(item.changes, matched->remote_endpoint != item.desired.remote_endpoint,
                       "remote_port");
            add_change(item.changes, !matched->managed_by_config, "ownership");
            item.endpoint_changed = matched->local_endpoint != item.desired.local_endpoint ||
                                    matched->remote_endpoint != item.desired.remote_endpoint;
            item.desired_state_changed = matched->desired_state != item.desired.desired_state;
            add_change(item.changes, item.desired_state_changed, "enabled");
            if (!item.changes.empty()) {
                item.mutation = item.desired.desired_state == TunnelDesiredState::disabled &&
                                        matched->desired_state != TunnelDesiredState::disabled
                                    ? MutationKind::disable
                                    : MutationKind::update;
            }
        }
        prepared.tunnels.push_back(std::move(item));
    }

    if (prune) {
        for (const auto& tunnel : *current_tunnels) {
            if (tunnel.managed_by_config && !matched_tunnel_ids.contains(tunnel.id.str())) {
                PreparedTunnel item{.original = tunnel,
                                    .desired = tunnel,
                                    .action_id = tunnel.id.str(),
                                    .mutation = MutationKind::remove,
                                    .changes = {},
                                    .endpoint_changed = false,
                                    .desired_state_changed = false};
                item.changes.emplace_back("resource");
                prepared.tunnels.push_back(std::move(item));
            }
        }
        for (const auto& server : *current_servers) {
            if (!server.managed_by_config || matched_server_ids.contains(server.id.str())) {
                continue;
            }
            const bool has_unmanaged_child = std::any_of(
                current_tunnels->begin(), current_tunnels->end(), [&server](const auto& tunnel) {
                    return tunnel.server_id == server.id && !tunnel.managed_by_config;
                });
            if (has_unmanaged_child) {
                return Error{ErrorCode::invalid_argument,
                             "prune would cascade into an imperatively managed tunnel"};
            }
            PreparedServer item{.original = server,
                                .desired = server,
                                .action_id = server.id.str(),
                                .credentials = {},
                                .mutation = MutationKind::remove,
                                .changes = {},
                                .transport_changed = false,
                                .desired_state_changed = false};
            item.changes.emplace_back("resource");
            prepared.servers.push_back(std::move(item));
        }
    }

    for (const auto& server : prepared.servers) {
        if (server.mutation != MutationKind::none) {
            prepared.actions.push_back(action_json(server.mutation, "server",
                                                   server.action_id, server.desired.name,
                                                   server.changes));
        }
    }
    for (const auto& tunnel : prepared.tunnels) {
        if (tunnel.mutation != MutationKind::none) {
            prepared.actions.push_back(action_json(tunnel.mutation, "tunnel",
                                                   tunnel.action_id, tunnel.desired.name,
                                                   tunnel.changes));
        }
    }
    auto& actions = prepared.actions.get_ref<Json::array_t&>();
    std::sort(actions.begin(), actions.end(), [](const Json& left, const Json& right) {
        const auto key = [](const Json& value) {
            std::array<std::string, 3> result;
            if (!value.is_object()) {
                return result;
            }
            const auto resource = value.find("resource");
            const auto id = value.find("id");
            const auto name = value.find("name");
            const auto action = value.find("action");
            if (resource != value.end() && resource->is_string()) {
                result[0] = resource->get<std::string>();
            }
            if (id != value.end() && id->is_string()) {
                result[1] = id->get<std::string>();
            } else if (name != value.end() && name->is_string()) {
                result[1] = name->get<std::string>();
            }
            if (action != value.end() && action->is_string()) {
                result[2] = action->get<std::string>();
            }
            return result;
        };
        return key(left) < key(right);
    });
    return prepared;
}

class StagedCredentialSet final {
  public:
    explicit StagedCredentialSet(storage::CredentialStore& credentials) noexcept
        : credentials_(credentials) {}
    ~StagedCredentialSet() noexcept {
        if (!released_) {
            for (const auto& key : keys_) {
                static_cast<void>(credentials_.remove(key));
            }
        }
    }
    [[nodiscard]] Result<void> put(const std::string& key, const std::string_view value) {
        try {
            keys_.push_back(key);
        } catch (...) {
            return Error{ErrorCode::resource_exhausted,
                         "insufficient memory while staging declarative credentials"};
        }
        auto stored = credentials_.put(key, value);
        if (!stored) {
            keys_.pop_back();
            return stored.error();
        }
        return Result<void>::success();
    }
    void release() noexcept { released_ = true; }

  private:
    storage::CredentialStore& credentials_;
    std::vector<std::string> keys_;
    bool released_{false};
};

void set_credential_reference(ServerRecord& server, const ServerCredentialKind kind,
                              std::optional<std::string> reference) {
    switch (kind) {
    case ServerCredentialKind::psk:
        server.credential_ref = std::move(reference);
        break;
    case ServerCredentialKind::ca_certificate:
        server.ca_credential_ref = std::move(reference);
        break;
    case ServerCredentialKind::client_certificate:
        server.client_certificate_ref = std::move(reference);
        break;
    case ServerCredentialKind::client_private_key:
        server.client_private_key_ref = std::move(reference);
        break;
    }
}

[[nodiscard]] Result<void> increment_revision(std::uint64_t& revision) {
    if (revision >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Error{ErrorCode::resource_exhausted, "configuration revision is exhausted"};
    }
    ++revision;
    return Result<void>::success();
}

} // namespace

DeclarativeConfig::DeclarativeConfig(storage::StateRepository& repository,
                                     storage::CredentialStore& credentials) noexcept
    : repository_(repository), credentials_(credentials) {}

Result<Json> DeclarativeConfig::export_config() const {
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
    std::sort(servers->begin(), servers->end(), [](const auto& left, const auto& right) {
        return left.id.str() < right.id.str();
    });
    std::sort(tunnels->begin(), tunnels->end(), [](const auto& left, const auto& right) {
        return left.id.str() < right.id.str();
    });
    Json exported_servers = Json::array();
    for (const auto& server : *servers) {
        if (server.desired_state == ServerDesiredState::removed) {
            continue;
        }
        exported_servers.push_back({
            {"id", server.id.str()},
            {"name", server.name.has_value() ? Json(*server.name) : Json(nullptr)},
            {"endpoint", server.endpoint.to_string()},
            {"tls_server_name",
             server.tls_server_name.has_value() ? Json(*server.tls_server_name) : Json(nullptr)},
            {"enabled", server.desired_state == ServerDesiredState::enabled},
            {"credentials",
             Json{{"psk", server.credential_ref.has_value()},
                  {"ca", server.ca_credential_ref.has_value()},
                  {"client_certificate", server.client_certificate_ref.has_value() &&
                                             server.client_private_key_ref.has_value()}}},
        });
    }
    Json exported_tunnels = Json::array();
    for (const auto& tunnel : *tunnels) {
        if (tunnel.desired_state == TunnelDesiredState::removed) {
            continue;
        }
        exported_tunnels.push_back({
            {"id", tunnel.id.str()},
            {"name", tunnel.name.has_value() ? Json(*tunnel.name) : Json(nullptr)},
            {"server", tunnel.server_id.str()},
            {"local_host", tunnel.local_endpoint.host()},
            {"local_port", tunnel.local_endpoint.port()},
            {"remote_port", tunnel.remote_endpoint.port()},
            {"enabled", tunnel.desired_state == TunnelDesiredState::active},
        });
    }
    return Json{{"format_version", 1U},
                {"servers", std::move(exported_servers)},
                {"tunnels", std::move(exported_tunnels)}};
}

Result<Json> DeclarativeConfig::plan(const std::string_view path, const bool prune) const {
    auto prepared = prepare_plan(repository_, credentials_, path, prune);
    if (!prepared) {
        return prepared.error();
    }
    const std::size_t action_count = prepared->actions.size();
    return Json{{"actions", std::move(prepared->actions)},
                {"changed", action_count},
                {"prune", prune}};
}

Result<Json> DeclarativeConfig::apply(const std::string_view path, const bool prune) {
    auto prepared = prepare_plan(repository_, credentials_, path, prune);
    if (!prepared) {
        return prepared.error();
    }
    if (prepared->actions.empty()) {
        return Json{{"actions", Json::array()}, {"changed", 0U}, {"prune", prune}};
    }

    StagedCredentialSet staged{credentials_};
    for (auto& server : prepared->servers) {
        if (server.mutation == MutationKind::none || server.mutation == MutationKind::remove) {
            continue;
        }
        for (auto& material : server.credentials) {
            if (!material.changed) {
                continue;
            }
            if (material.final_value.empty()) {
                set_credential_reference(server.desired, material.kind, std::nullopt);
                continue;
            }
            const std::string key = next_server_credential_key(server.desired, material.kind);
            auto stored = staged.put(key, material.final_value.view());
            if (!stored) {
                return stored.error();
            }
            set_credential_reference(server.desired, material.kind, key);
        }
    }

    auto transaction = repository_.begin_transaction();
    if (!transaction) {
        return transaction.error();
    }
    for (auto& server : prepared->servers) {
        if (server.mutation == MutationKind::none || server.mutation == MutationKind::remove) {
            continue;
        }
        if (!server.original.has_value()) {
            auto created = repository_.servers().create(server.desired, *transaction);
            if (!created) {
                return created.error();
            }
            continue;
        }
        auto live = repository_.servers().get_by_id(server.desired.id);
        if (!live || live->config_revision != server.original->config_revision ||
            live->desired_state == ServerDesiredState::removed) {
            return Error{ErrorCode::database_error,
                         "server configuration changed while declarative apply was preparing"};
        }
        const auto previous_actual = live->actual_state;
        live->name = server.desired.name;
        live->endpoint = server.desired.endpoint;
        live->tls_server_name = server.desired.tls_server_name;
        live->credential_ref = server.desired.credential_ref;
        live->ca_credential_ref = server.desired.ca_credential_ref;
        live->client_certificate_ref = server.desired.client_certificate_ref;
        live->client_private_key_ref = server.desired.client_private_key_ref;
        live->desired_state = server.desired.desired_state;
        live->managed_by_config = true;
        if (auto revision = increment_revision(live->config_revision); !revision) {
            return revision.error();
        }
        if (server.transport_changed || server.desired_state_changed) {
            live->actual_state = live->desired_state == ServerDesiredState::disabled
                                     ? ServerActualState::disabled
                                     : (live->credential_ref.has_value()
                                            ? ServerActualState::disconnected
                                            : ServerActualState::not_authenticated);
            live->remote_server_id.reset();
            live->last_error_code.reset();
            live->last_error_message.reset();
            live->reconnect_attempt = 0U;
            live->latency_ms.reset();
        } else {
            live->actual_state = previous_actual;
        }
        live->updated_at_unix_ms = next_update_time(live->updated_at_unix_ms);
        auto updated = repository_.servers().update(*live, *transaction);
        if (!updated) {
            return updated.error();
        }
        server.desired = std::move(*live);
    }

    for (auto& tunnel : prepared->tunnels) {
        if (tunnel.mutation == MutationKind::none || tunnel.mutation == MutationKind::remove) {
            continue;
        }
        if (!tunnel.original.has_value()) {
            auto created = repository_.tunnels().create(tunnel.desired, *transaction);
            if (!created) {
                return created.error();
            }
            continue;
        }
        auto live = repository_.tunnels().get_by_id(tunnel.desired.id);
        if (!live || live->config_revision != tunnel.original->config_revision ||
            live->desired_state == TunnelDesiredState::removed) {
            return Error{ErrorCode::database_error,
                         "tunnel configuration changed while declarative apply was preparing"};
        }
        live->name = tunnel.desired.name;
        live->local_endpoint = tunnel.desired.local_endpoint;
        live->remote_endpoint = tunnel.desired.remote_endpoint;
        live->desired_state = tunnel.desired.desired_state;
        live->managed_by_config = true;
        if (auto revision = increment_revision(live->config_revision); !revision) {
            return revision.error();
        }
        if (tunnel.endpoint_changed || tunnel.desired_state_changed) {
            live->actual_state = live->desired_state == TunnelDesiredState::active
                                     ? TunnelActualState::pending
                                     : TunnelActualState::disabled;
            live->last_error_code.reset();
            live->last_error_message.reset();
        }
        live->updated_at_unix_ms = next_update_time(live->updated_at_unix_ms);
        auto updated = repository_.tunnels().update(*live, *transaction);
        if (!updated) {
            return updated.error();
        }
        tunnel.desired = std::move(*live);
    }

    for (const auto& tunnel : prepared->tunnels) {
        if (tunnel.mutation != MutationKind::remove) {
            continue;
        }
        auto live = repository_.tunnels().get_by_id(tunnel.desired.id);
        if (!live || !tunnel.original.has_value() ||
            live->config_revision != tunnel.original->config_revision) {
            return Error{ErrorCode::database_error,
                         "tunnel configuration changed while declarative prune was preparing"};
        }
        auto removed = repository_.tunnels().mark_removed(
            live->id, next_update_time(live->updated_at_unix_ms), *transaction);
        if (!removed) {
            return removed.error();
        }
    }
    for (const auto& server : prepared->servers) {
        if (server.mutation != MutationKind::remove) {
            continue;
        }
        auto live = repository_.servers().get_by_id(server.desired.id);
        if (!live || !server.original.has_value() ||
            live->config_revision != server.original->config_revision) {
            return Error{ErrorCode::database_error,
                         "server configuration changed while declarative prune was preparing"};
        }
        auto removed = repository_.servers().mark_removed(
            live->id, next_update_time(live->updated_at_unix_ms), *transaction);
        if (!removed) {
            return removed.error();
        }
    }

    auto committed = transaction->commit();
    if (!committed) {
        return committed.error();
    }
    staged.release();
    for (const auto& server : prepared->servers) {
        if (server.mutation == MutationKind::none || server.mutation == MutationKind::remove) {
            continue;
        }
        for (const auto& material : server.credentials) {
            if (!material.changed) {
                continue;
            }
            const auto& retained = credential_reference(server.desired, material.kind);
            auto cleaned = cleanup_server_credential_kind(
                credentials_, server.desired.id, material.kind,
                material.previous_reference.has_value()
                    ? std::optional<std::string_view>{*material.previous_reference}
                    : std::nullopt,
                retained.has_value() ? std::optional<std::string_view>{*retained} : std::nullopt);
            static_cast<void>(cleaned);
        }
    }
    const std::size_t action_count = prepared->actions.size();
    return Json{{"actions", std::move(prepared->actions)},
                {"changed", action_count},
                {"prune", prune}};
}

} // namespace minitun::daemon
