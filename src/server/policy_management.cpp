#include <minitun/server/policy_management.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/secure_string.hpp>
#include <minitun/common/time.hpp>

namespace minitun::server {
namespace {

using Json = nlohmann::json;

using admin::ManagementRequest;
using admin::ManagementResponse;

using common::Error;
using common::ErrorCode;
using common::Result;

constexpr std::size_t kMaximumPskBytes = 64U * 1024U;
constexpr std::size_t kRandomPskBytes = 32U;
constexpr std::string_view kHexDigits{"0123456789abcdef"};
constexpr std::int64_t kMaximumGraceSeconds = 86'400;
constexpr std::int64_t kMinimumGraceSeconds = 1;

[[nodiscard]] std::string psk_file_for(const std::string_view client_id,
                                       const std::string_view suffix) {
    std::string name{client_id};
    name.append(suffix);
    return name;
}

[[nodiscard]] Result<std::string> generate_psk() {
    std::array<unsigned char, kRandomPskBytes> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        return common::Result<std::string>::failure(common::ErrorCode::internal_error,
                                                    "secure random PSK generation failed");
    }
    std::string hex;
    hex.reserve(kRandomPskBytes * 2U);
    for (const unsigned char byte : random) {
        hex.push_back(kHexDigits[byte >> 4U]);
        hex.push_back(kHexDigits[byte & 0x0fU]);
    }
    common::secure_erase_memory(random.data(), random.size());
    return hex;
}

class FileDescriptor final {
  public:
    explicit FileDescriptor(const int value) noexcept : value_(value) {}
    ~FileDescriptor() noexcept {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return value_; }

  private:
    int value_;
};

[[nodiscard]] Result<void> write_secret_file_atomic(const std::string& directory,
                                                    const std::string& filename,
                                                    const std::string_view secret) {
    if (secret.empty() || secret.size() > kMaximumPskBytes ||
        secret.find('\0') != std::string_view::npos) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "PSK content is invalid");
    }
    if (filename.empty() || filename.find('/') != std::string::npos ||
        filename.find('\0') != std::string::npos) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "PSK file name is invalid");
    }
    std::string pattern = directory + "/.minitun-psk-XXXXXX";
    std::vector<char> template_bytes{pattern.begin(), pattern.end()};
    template_bytes.push_back('\0');
    const int raw_descriptor = ::mkstemp(template_bytes.data());
    if (raw_descriptor < 0) {
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             "policy directory is not writable");
    }
    const FileDescriptor descriptor{raw_descriptor};
    const std::string temporary{template_bytes.begin(), template_bytes.end() - 1U};
    const std::string target = directory + "/" + filename;
    if (::fchmod(descriptor.get(), S_IRUSR | S_IWUSR) != 0) {
        static_cast<void>(::unlink(temporary.c_str()));
        return common::Result<void>::failure(common::ErrorCode::permission_denied,
                                             "PSK file mode cannot be applied");
    }
    std::string content{secret};
    content.push_back('\n');
    std::size_t offset = 0U;
    while (offset < content.size()) {
        const ssize_t count =
            ::write(descriptor.get(), content.data() + offset, content.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            static_cast<void>(::unlink(temporary.c_str()));
            common::secure_erase_memory(content.data(), content.size());
            return common::Result<void>::failure(common::ErrorCode::internal_error,
                                                 "PSK file write failed");
        }
        offset += static_cast<std::size_t>(count);
    }
    common::secure_erase_memory(content.data(), content.size());
    if (::fsync(descriptor.get()) != 0 || ::rename(temporary.c_str(), target.c_str()) != 0) {
        static_cast<void>(::unlink(temporary.c_str()));
        return common::Result<void>::failure(common::ErrorCode::internal_error,
                                             "PSK file replacement failed");
    }
    return common::Result<void>::success();
}

[[nodiscard]] Result<Json> load_document(const PolicyManagementBindings& bindings) {
    if (!bindings.document) {
        return common::Result<Json>::failure(common::ErrorCode::internal_error,
                                             "policy document provider is unavailable");
    }
    auto text = bindings.document();
    if (!text) {
        return common::Result<Json>::failure(text.error());
    }
    try {
        return Json::parse(*text);
    } catch (const Json::exception&) {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "policy document is not valid JSON");
    }
}

[[nodiscard]] Result<void> persist_document(const PolicyManagementBindings& bindings,
                                            const Json& document) {
    if (!bindings.replace) {
        return common::Result<void>::failure(common::ErrorCode::internal_error,
                                             "policy replacement provider is unavailable");
    }
    std::string text;
    try {
        text = document.dump(2);
    } catch (const Json::exception&) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "policy document cannot be serialized");
    } catch (const std::bad_alloc&) {
        return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                             "insufficient memory while serializing policies");
    }
    auto replaced = bindings.replace(std::move(text));
    if (!replaced) {
        return common::Result<void>::failure(replaced.error());
    }
    return common::Result<void>::success();
}

[[nodiscard]] Json client_summary(const Json& entry) {
    Json summary{
        {"client_id", entry.at("client_id")},
        {"enabled", entry.at("enabled")},
        {"psk_file", entry.at("psk_file")},
        {"allowed_ports", entry.at("allowed_ports")},
        {"max_tunnels", entry.at("max_tunnels")},
        {"max_connections", entry.at("max_connections")},
        {"max_idle_workers", entry.at("max_idle_workers")},
        {"connections_per_minute", entry.value("connections_per_minute", 0U)},
        {"rotation_active", entry.contains("previous_psk_file")},
    };
    if (entry.contains("certificate_sha256")) {
        summary["certificate_sha256"] = entry.at("certificate_sha256");
    }
    if (entry.contains("certificate_san")) {
        summary["certificate_san"] = entry.at("certificate_san");
    }
    if (entry.contains("allowed_source_cidrs")) {
        summary["allowed_source_cidrs"] = entry.at("allowed_source_cidrs");
    }
    if (entry.contains("previous_psk_expires_at")) {
        summary["previous_psk_expires_at"] = entry.at("previous_psk_expires_at");
    }
    return summary;
}

[[nodiscard]] Result<ManagementResponse> response_json(Json body,
                                                       const unsigned int status = 200U) {
    std::string text;
    try {
        text = body.dump();
    } catch (const Json::exception&) {
        return common::Result<ManagementResponse>::failure(
            common::ErrorCode::internal_error, "management response cannot be serialized");
    }
    return ManagementResponse{status, "OK", "application/json", std::move(text)};
}

[[nodiscard]] Result<std::vector<std::string>> split_path(const std::string_view path) {
    std::vector<std::string> segments;
    if (path.empty() || path.front() != '/') {
        return common::Result<std::vector<std::string>>::failure(
            common::ErrorCode::invalid_argument, "management path is invalid");
    }
    std::size_t cursor = 1U;
    while (cursor <= path.size()) {
        const auto slash = path.find('/', cursor);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (end == cursor) {
            return common::Result<std::vector<std::string>>::failure(
                common::ErrorCode::invalid_argument, "management path is invalid");
        }
        segments.emplace_back(path.substr(cursor, end - cursor));
        cursor = end + 1U;
    }
    return segments;
}

[[nodiscard]] Result<void> validate_client_id(const std::string_view value) {
    auto parsed = common::Id::parse(value, common::IdKind::client);
    if (!parsed) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "client_id is invalid");
    }
    return common::Result<void>::success();
}

[[nodiscard]] Result<Json> parse_request_body(const std::string_view body) {
    if (body.empty()) {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "request body is required");
    }
    if (body.size() > kMaximumPskBytes * 4U) {
        return common::Result<Json>::failure(common::ErrorCode::resource_exhausted,
                                             "request body exceeds the management limit");
    }
    try {
        auto parsed = Json::parse(body);
        if (!parsed.is_object()) {
            return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                                 "request body must be a JSON object");
        }
        return parsed;
    } catch (const Json::exception&) {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "request body is not strict JSON");
    }
}

[[nodiscard]] Result<void>
validate_upsert_body(const Json& body, const std::string_view path_id) {
    static const std::set<std::string_view> allowed_fields{
        "client_id",          "enabled",         "psk",                "allowed_ports",
        "max_tunnels",        "max_connections", "max_idle_workers",   "certificate_sha256",
        "certificate_san",    "allowed_source_cidrs", "connections_per_minute",
    };
    for (auto item = body.cbegin(); item != body.cend(); ++item) {
        if (!allowed_fields.contains(item.key())) {
            return common::Result<void>::failure(
                common::ErrorCode::invalid_argument,
                "client policy body contains an unknown field");
        }
    }
    if (body.contains("client_id")) {
        const auto& value = body.at("client_id");
        if (!value.is_string() || value.get_ref<const std::string&>() != path_id) {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "client_id must match the request path");
        }
    }
    if (body.contains("enabled") && !body.at("enabled").is_boolean()) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "enabled must be a boolean");
    }
    if (body.contains("psk")) {
        const auto& value = body.at("psk");
        if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
            value.get_ref<const std::string&>().size() > kMaximumPskBytes ||
            value.get_ref<const std::string&>().find('\0') != std::string::npos) {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "psk must be a bounded string");
        }
    }
    for (const std::string_view field : {"allowed_ports", "allowed_source_cidrs"}) {
        if (!body.contains(field)) {
            continue;
        }
        const auto& value = body.at(std::string{field});
        if (!value.is_array() || value.empty()) {
            return common::Result<void>::failure(
                common::ErrorCode::invalid_argument,
                std::string{field} + " must be a non-empty array");
        }
        for (const auto& entry : value) {
            if (!entry.is_string()) {
                return common::Result<void>::failure(
                    common::ErrorCode::invalid_argument,
                    std::string{field} + " entries must be strings");
            }
        }
    }
    for (const std::string_view field : {"max_tunnels", "max_connections", "max_idle_workers",
                                         "connections_per_minute"}) {
        if (!body.contains(field)) {
            continue;
        }
        const auto& value = body.at(std::string{field});
        if (!value.is_number_unsigned() ||
            value.get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return common::Result<void>::failure(
                common::ErrorCode::invalid_argument,
                std::string{field} + " must be an unsigned integer");
        }
    }
    if (body.contains("certificate_sha256") && body.contains("certificate_san")) {
        return common::Result<void>::failure(
            common::ErrorCode::invalid_argument,
            "certificate_sha256 and certificate_san are mutually exclusive");
    }
    for (const std::string_view field : {"certificate_sha256", "certificate_san"}) {
        if (body.contains(field) && !body.at(std::string{field}).is_string()) {
            return common::Result<void>::failure(
                common::ErrorCode::invalid_argument,
                std::string{field} + " must be a string");
        }
    }
    return common::Result<void>::success();
}

[[nodiscard]] Json* find_client(Json& document, const std::string_view client_id) {
    auto clients = document.find("clients");
    if (clients == document.end() || !clients->is_array()) {
        return nullptr;
    }
    for (auto& entry : *clients) {
        if (entry.is_object() && entry.contains("client_id") &&
            entry.at("client_id").is_string() &&
            entry.at("client_id").get_ref<const std::string&>() == client_id) {
            return &entry;
        }
    }
    return nullptr;
}

class Handler final {
  public:
    Handler(PolicyManagementBindings bindings, PolicyManagementOptions options)
        : bindings_(std::move(bindings)), options_(options) {}

    Result<ManagementResponse> operator()(const ManagementRequest& request) const {
        auto segments = split_path(request.path);
        if (!segments) {
            return common::Result<ManagementResponse>::failure(segments.error());
        }
        if (segments->size() == 2U && segments->at(0U) == "v1" &&
            segments->at(1U) == "health") {
            return handle_health(request);
        }
        if (segments->size() == 2U && segments->at(0U) == "v1" &&
            segments->at(1U) == "reload") {
            return handle_reload(request);
        }
        if (segments->size() == 2U && segments->at(0U) == "v1" &&
            segments->at(1U) == "clients") {
            return handle_clients(request, *segments);
        }
        if (segments->size() >= 3U && segments->at(0U) == "v1" &&
            segments->at(1U) == "clients") {
            return handle_clients(request, *segments);
        }
        return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                           "management endpoint not found");
    }

  private:
    Result<ManagementResponse> handle_health(const ManagementRequest& request) const {
        if (request.method != "GET") {
            return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                               "management endpoint not found");
        }
        const std::string server_id = bindings_.server_id ? bindings_.server_id() : std::string{};
        return response_json(Json{{"status", "ok"}, {"server_id", server_id}});
    }

    Result<ManagementResponse> handle_reload(const ManagementRequest& request) const {
        if (request.method != "POST") {
            return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                               "management endpoint not found");
        }
        if (!request.body.empty()) {
            return common::Result<ManagementResponse>::failure(common::ErrorCode::invalid_argument,
                                                               "reload takes no request body");
        }
        if (!bindings_.reload) {
            return common::Result<ManagementResponse>::failure(common::ErrorCode::internal_error,
                                                               "policy reload is unavailable");
        }
        auto changed = bindings_.reload();
        if (!changed) {
            return common::Result<ManagementResponse>::failure(changed.error());
        }
        return response_json(Json{{"changed", *changed}});
    }

    Result<ManagementResponse> handle_clients(const ManagementRequest& request,
                                              const std::vector<std::string>& segments) const {
        if (segments.size() == 2U) {
            if (request.method == "GET") {
                return list_clients();
            }
            return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                               "management endpoint not found");
        }
        if (segments.size() != 3U && segments.size() != 4U) {
            return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                               "management endpoint not found");
        }
        const std::string_view client_id = segments.at(2U);
        if (auto valid = validate_client_id(client_id); !valid) {
            return common::Result<ManagementResponse>::failure(valid.error());
        }
        if (segments.size() == 4U) {
            if (segments.at(3U) != "rotate-psk" || request.method != "POST") {
                return common::Result<ManagementResponse>::failure(
                    common::ErrorCode::not_found, "management endpoint not found");
            }
            return rotate_psk(client_id, request.body);
        }
        if (request.method == "GET") {
            return get_client(client_id);
        }
        if (request.method == "PUT") {
            return upsert_client(client_id, request.body);
        }
        if (request.method == "DELETE") {
            return delete_client(client_id);
        }
        return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                           "management endpoint not found");
    }

    Result<ManagementResponse> list_clients() const {
        auto document = load_document(bindings_);
        if (!document) {
            return common::Result<ManagementResponse>::failure(document.error());
        }
        Json summaries = Json::array();
        for (const auto& entry : document->at("clients")) {
            summaries.push_back(client_summary(entry));
        }
        return response_json(Json{{"clients", std::move(summaries)}});
    }

    Result<ManagementResponse> get_client(const std::string_view client_id) const {
        auto document = load_document(bindings_);
        if (!document) {
            return common::Result<ManagementResponse>::failure(document.error());
        }
        const auto* entry = find_client(*document, client_id);
        if (entry == nullptr) {
            return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                               "client policy not found");
        }
        return response_json(Json{{"client", client_summary(*entry)}});
    }

    Result<ManagementResponse> upsert_client(const std::string_view client_id,
                                             const std::string_view body_text) const {
        auto body = parse_request_body(body_text);
        if (!body) {
            return common::Result<ManagementResponse>::failure(body.error());
        }
        if (auto valid = validate_upsert_body(*body, client_id); !valid) {
            return common::Result<ManagementResponse>::failure(valid.error());
        }
        auto document = load_document(bindings_);
        if (!document) {
            return common::Result<ManagementResponse>::failure(document.error());
        }
        if (document->at("clients").size() >= 100'000U) {
            return common::Result<ManagementResponse>::failure(
                common::ErrorCode::resource_exhausted, "client policy limit reached");
        }
        const std::string directory =
            bindings_.config_directory ? bindings_.config_directory() : std::string{};
        if (directory.empty()) {
            return common::Result<ManagementResponse>::failure(
                common::ErrorCode::internal_error, "policy directory is unavailable");
        }
        Json* existing = find_client(*document, client_id);
        const bool create = existing == nullptr;
        if (create) {
            for (const std::string_view field :
                 {"allowed_ports", "max_tunnels", "max_connections", "max_idle_workers"}) {
                if (!body->contains(field)) {
                    return common::Result<ManagementResponse>::failure(
                        common::ErrorCode::invalid_argument,
                        std::string{field} + " is required when creating a client policy");
                }
            }
        }
        std::optional<std::string> generated_psk;
        if (body->contains("psk")) {
            auto written = write_secret_file_atomic(
                directory, psk_file_for(client_id, ".psk"),
                body->at("psk").get_ref<const std::string&>());
            if (!written) {
                return common::Result<ManagementResponse>::failure(written.error());
            }
            if (existing != nullptr) {
                existing->erase("previous_psk_file");
                existing->erase("previous_psk_expires_at");
            }
        } else if (create) {
            auto generated = generate_psk();
            if (!generated) {
                return common::Result<ManagementResponse>::failure(generated.error());
            }
            auto written =
                write_secret_file_atomic(directory, psk_file_for(client_id, ".psk"), *generated);
            if (!written) {
                common::secure_erase_memory(generated->data(), generated->size());
                return common::Result<ManagementResponse>::failure(written.error());
            }
            generated_psk = std::move(*generated);
        }
        Json& entry = existing == nullptr ? document->at("clients").emplace_back() : *existing;
        entry["client_id"] = std::string{client_id};
        if (body->contains("psk") || create) {
            entry["psk_file"] = psk_file_for(client_id, ".psk");
        }
        entry["enabled"] = body->value("enabled", create ? true : entry.value("enabled", true));
        const auto copy_field = [&](const std::string_view field) {
            if (body->contains(field)) {
                entry[std::string{field}] = body->at(std::string{field});
            }
        };
        copy_field("allowed_ports");
        copy_field("max_tunnels");
        copy_field("max_connections");
        copy_field("max_idle_workers");
        copy_field("connections_per_minute");
        copy_field("allowed_source_cidrs");
        if (body->contains("certificate_sha256") || body->contains("certificate_san")) {
            entry.erase("certificate_sha256");
            entry.erase("certificate_san");
            copy_field("certificate_sha256");
            copy_field("certificate_san");
        }
        if (auto persisted = persist_document(bindings_, *document); !persisted) {
            return common::Result<ManagementResponse>::failure(persisted.error());
        }
        Json result{{"client", client_summary(entry)}};
        if (generated_psk.has_value()) {
            result["psk"] = *generated_psk;
            common::secure_erase_memory(generated_psk->data(), generated_psk->size());
        }
        return response_json(std::move(result));
    }

    Result<ManagementResponse> delete_client(const std::string_view client_id) const {
        auto document = load_document(bindings_);
        if (!document) {
            return common::Result<ManagementResponse>::failure(document.error());
        }
        auto& clients = document->at("clients");
        std::size_t index = 0U;
        bool found = false;
        for (; index < clients.size(); ++index) {
            const auto& entry = clients.at(index);
            if (entry.is_object() && entry.contains("client_id") &&
                entry.at("client_id").is_string() &&
                entry.at("client_id").get_ref<const std::string&>() == client_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                               "client policy not found");
        }
        clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
        if (auto persisted = persist_document(bindings_, *document); !persisted) {
            return common::Result<ManagementResponse>::failure(persisted.error());
        }
        return response_json(Json{{"removed", std::string{client_id}}});
    }

    Result<ManagementResponse> rotate_psk(const std::string_view client_id,
                                          const std::string_view body_text) const {
        std::int64_t grace = options_.rotation_grace_seconds;
        if (!body_text.empty()) {
            auto body = parse_request_body(body_text);
            if (!body) {
                return common::Result<ManagementResponse>::failure(body.error());
            }
            if (body->size() != 1U || !body->contains("grace_seconds")) {
                return common::Result<ManagementResponse>::failure(
                    common::ErrorCode::invalid_argument,
                    "rotation body accepts only grace_seconds");
            }
            const auto& value = body->at("grace_seconds");
            if (!value.is_number_unsigned() ||
                value.get<std::uint64_t>() >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return common::Result<ManagementResponse>::failure(
                    common::ErrorCode::invalid_argument, "grace_seconds is invalid");
            }
            grace = static_cast<std::int64_t>(value.get<std::uint64_t>());
        }
        if (grace < kMinimumGraceSeconds || grace > kMaximumGraceSeconds) {
            return common::Result<ManagementResponse>::failure(
                common::ErrorCode::invalid_argument,
                "grace_seconds must be between 1 and 86400");
        }
        auto document = load_document(bindings_);
        if (!document) {
            return common::Result<ManagementResponse>::failure(document.error());
        }
        Json* entry = find_client(*document, client_id);
        if (entry == nullptr) {
            return common::Result<ManagementResponse>::failure(common::ErrorCode::not_found,
                                                               "client policy not found");
        }
        if (!entry->contains("psk_file") || !entry->at("psk_file").is_string() ||
            entry->at("psk_file").get_ref<const std::string&>().empty()) {
            return common::Result<ManagementResponse>::failure(
                common::ErrorCode::invalid_argument, "client policy has no PSK file");
        }
        const std::string directory =
            bindings_.config_directory ? bindings_.config_directory() : std::string{};
        if (directory.empty()) {
            return common::Result<ManagementResponse>::failure(
                common::ErrorCode::internal_error, "policy directory is unavailable");
        }
        auto generated = generate_psk();
        if (!generated) {
            return common::Result<ManagementResponse>::failure(generated.error());
        }
        // Alternate between the two canonical file names so the file that
        // still holds the outgoing secret becomes the rotation predecessor.
        const std::string current_file = entry->at("psk_file").get<std::string>();
        const std::string canonical = psk_file_for(client_id, ".psk");
        const std::string previous = psk_file_for(client_id, ".psk.previous");
        const std::string target = current_file == canonical ? previous : canonical;
        auto written = write_secret_file_atomic(directory, target, *generated);
        if (!written) {
            common::secure_erase_memory(generated->data(), generated->size());
            return common::Result<ManagementResponse>::failure(written.error());
        }
        (*entry)["previous_psk_file"] = current_file;
        (*entry)["previous_psk_expires_at"] = common::unix_seconds_now() + grace;
        (*entry)["psk_file"] = target;
        if (auto persisted = persist_document(bindings_, *document); !persisted) {
            common::secure_erase_memory(generated->data(), generated->size());
            return common::Result<ManagementResponse>::failure(persisted.error());
        }
        const std::int64_t expires_at = entry->at("previous_psk_expires_at").get<std::int64_t>();
        Json result{{"psk", *generated}, {"previous_psk_expires_at", expires_at}};
        common::secure_erase_memory(generated->data(), generated->size());
        return response_json(std::move(result));
    }

    PolicyManagementBindings bindings_;
    PolicyManagementOptions options_;
};

} // namespace

common::Result<std::function<common::Result<ManagementResponse>(const ManagementRequest&)>>
make_policy_management_handler(PolicyManagementBindings bindings,
                               PolicyManagementOptions options) {
    if (options.rotation_grace_seconds < kMinimumGraceSeconds ||
        options.rotation_grace_seconds > kMaximumGraceSeconds) {
        return common::Result<std::function<common::Result<ManagementResponse>(
            const ManagementRequest&)>>::failure(
            common::ErrorCode::invalid_argument, "rotation grace window is invalid");
    }
    return std::function<common::Result<ManagementResponse>(const ManagementRequest&)>{
        Handler{std::move(bindings), options}};
}

} // namespace minitun::server
