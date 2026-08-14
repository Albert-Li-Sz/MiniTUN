#include <minitun/server/client_policy.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>

namespace minitun::server {
namespace {

using Json = nlohmann::json;

inline constexpr std::size_t kMaxPathBytes = 4'096U;
inline constexpr std::size_t kMaxPolicyBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxPskBytes = 64U * 1024U;
inline constexpr std::size_t kMaxJsonDepth = 8U;
inline constexpr std::size_t kMaxJsonNodes = 20'000U;
inline constexpr std::size_t kMaxJsonStringBytes = 4'096U;
inline constexpr std::size_t kSha256HexBytes = 64U;
inline constexpr std::string_view kHexDigits{"0123456789abcdef"};

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

enum class FileKind : std::uint8_t {
    policy,
    secret,
};

[[nodiscard]] common::Result<std::vector<char>> read_secure_file(const std::string& path,
                                                                 const FileKind kind) {
    const std::size_t limit = kind == FileKind::secret ? kMaxPskBytes : kMaxPolicyBytes;
    const std::string_view description = kind == FileKind::secret ? "client PSK" : "client policy";
    if (path.empty() || path.size() > kMaxPathBytes || path.find('\0') != std::string::npos) {
        return common::Result<std::vector<char>>::failure(
            common::ErrorCode::invalid_argument, std::string{description} + " path is invalid");
    }

    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int raw_descriptor = ::open(path.c_str(), flags);
    if (raw_descriptor < 0) {
        return common::Result<std::vector<char>>::failure(
            errno == EACCES ? common::ErrorCode::permission_denied
                            : common::ErrorCode::invalid_argument,
            std::string{description} + " file cannot be opened");
    }
    const FileDescriptor descriptor{raw_descriptor};

    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1) {
        return common::Result<std::vector<char>>::failure(
            common::ErrorCode::permission_denied,
            std::string{description} + " file must be one regular, non-linked file");
    }
    const mode_t forbidden_mode = kind == FileKind::secret ? 0077 : 0022;
    if (status.st_uid != ::geteuid() || (status.st_mode & forbidden_mode) != 0) {
        return common::Result<std::vector<char>>::failure(
            common::ErrorCode::permission_denied,
            kind == FileKind::secret
                ? "client PSK file must be owned by the server user and inaccessible to others"
                : "client policy file must be owned by the server user and not writable by others");
    }
    if (status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > limit) {
        return common::Result<std::vector<char>>::failure(
            common::ErrorCode::invalid_argument, std::string{description} + " file size is invalid");
    }

    std::vector<char> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (kind == FileKind::secret) {
                common::secure_erase_memory(bytes.data(), bytes.size());
            }
            return common::Result<std::vector<char>>::failure(
                common::ErrorCode::invalid_argument,
                std::string{description} + " file could not be read completely");
        }
        offset += static_cast<std::size_t>(count);
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
    bool object;
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
            if (contexts.size() >= kMaxJsonDepth) {
                failure = ParseFailure::depth;
                return false;
            }
            if (++nodes > kMaxJsonNodes) {
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
            if (++nodes > kMaxJsonNodes) {
                failure = ParseFailure::nodes;
                return false;
            }
            const auto& key = parsed.get_ref<const std::string&>();
            if (key.size() > kMaxJsonStringBytes) {
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
            if (++nodes > kMaxJsonNodes) {
                failure = ParseFailure::nodes;
                return false;
            }
            if (parsed.is_string() &&
                parsed.get_ref<const std::string&>().size() > kMaxJsonStringBytes) {
                failure = ParseFailure::string;
                return false;
            }
        }
        return true;
    }
};

[[nodiscard]] common::Result<Json> parse_policy_json(const std::vector<char>& bytes) {
    if (bytes.size() >= 3U && static_cast<unsigned char>(bytes[0]) == 0xefU &&
        static_cast<unsigned char>(bytes[1]) == 0xbbU &&
        static_cast<unsigned char>(bytes[2]) == 0xbfU) {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "client policy JSON must not contain a UTF-8 BOM");
    }
    ParseLimits limits;
    const auto callback = [&limits](const int, const Json::parse_event_t event, Json& parsed) {
        return limits.accept(event, parsed);
    };
    try {
        auto document = Json::parse(bytes.cbegin(), bytes.cend(), callback, true, false);
        switch (limits.failure) {
        case ParseFailure::duplicate_key:
            return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                                 "client policy JSON contains a duplicate key");
        case ParseFailure::depth:
        case ParseFailure::nodes:
        case ParseFailure::string:
            return common::Result<Json>::failure(common::ErrorCode::resource_exhausted,
                                                 "client policy JSON exceeds a parsing limit");
        case ParseFailure::none:
            return document;
        }
    } catch (const Json::exception&) {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "client policy file is not strict JSON");
    } catch (const std::bad_alloc&) {
        return common::Result<Json>::failure(common::ErrorCode::resource_exhausted,
                                             "insufficient memory while loading client policy");
    }
    return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                         "client policy file is not strict JSON");
}

[[nodiscard]] bool has_exact_fields(const Json& object,
                                    const std::set<std::string_view>& allowed) {
    if (!object.is_object()) {
        return false;
    }
    for (auto item = object.cbegin(); item != object.cend(); ++item) {
        if (!allowed.contains(item.key())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] common::Result<std::size_t>
parse_limit(const Json& value, const std::size_t maximum, const std::string_view name) {
    if (!value.is_number_unsigned()) {
        return common::Result<std::size_t>::failure(common::ErrorCode::invalid_argument,
                                                   std::string{name} + " must be an unsigned integer");
    }
    const auto number = value.get<std::uint64_t>();
    if (number == 0U || number > maximum || number > std::numeric_limits<std::size_t>::max()) {
        return common::Result<std::size_t>::failure(common::ErrorCode::invalid_argument,
                                                   std::string{name} + " is outside the server limit");
    }
    return static_cast<std::size_t>(number);
}

[[nodiscard]] bool is_lower_hex(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

[[nodiscard]] common::Result<ClientCertificateBinding>
parse_certificate_binding(const Json& client) {
    const bool has_sha256 = client.contains("certificate_sha256");
    const bool has_san = client.contains("certificate_san");
    if (has_sha256 && has_san) {
        return common::Result<ClientCertificateBinding>::failure(
            common::ErrorCode::invalid_argument,
            "a client policy may bind either a certificate fingerprint or SAN, not both");
    }
    if (has_sha256) {
        const auto& value = client.at("certificate_sha256");
        if (!value.is_string()) {
            return common::Result<ClientCertificateBinding>::failure(
                common::ErrorCode::invalid_argument,
                "certificate_sha256 must be a lowercase hexadecimal string");
        }
        const auto fingerprint = value.get<std::string>();
        if (fingerprint.size() != kSha256HexBytes || !is_lower_hex(fingerprint)) {
            return common::Result<ClientCertificateBinding>::failure(
                common::ErrorCode::invalid_argument,
                "certificate_sha256 must contain 64 lowercase hexadecimal characters");
        }
        return ClientCertificateBinding{ClientCertificateBindingKind::sha256, fingerprint};
    }
    if (has_san) {
        const auto& value = client.at("certificate_san");
        if (!value.is_string()) {
            return common::Result<ClientCertificateBinding>::failure(
                common::ErrorCode::invalid_argument, "certificate_san must be a string");
        }
        const auto san = value.get<std::string>();
        constexpr std::array<std::string_view, 4U> prefixes{"DNS:", "URI:", "EMAIL:", "IP:"};
        const bool valid = san.size() <= 1'024U &&
                           std::any_of(prefixes.begin(), prefixes.end(), [&san](const auto prefix) {
                               return san.starts_with(prefix) && san.size() > prefix.size();
                           });
        if (!valid || san.find('\0') != std::string::npos) {
            return common::Result<ClientCertificateBinding>::failure(
                common::ErrorCode::invalid_argument,
                "certificate_san must use a non-empty DNS:, URI:, EMAIL:, or IP: value");
        }
        return ClientCertificateBinding{ClientCertificateBindingKind::san, san};
    }
    return ClientCertificateBinding{};
}

[[nodiscard]] common::Result<std::shared_ptr<const common::SecureString>>
load_psk(const std::filesystem::path& config_directory, const std::string& configured_path) {
    if (configured_path.empty() || configured_path.size() > kMaxPathBytes ||
        configured_path.find('\0') != std::string::npos) {
        return common::Result<std::shared_ptr<const common::SecureString>>::failure(
            common::ErrorCode::invalid_argument, "psk_file path is invalid");
    }
    std::filesystem::path path{configured_path};
    if (path.is_relative()) {
        path = config_directory / path;
    }
    auto bytes = read_secure_file(path.lexically_normal().string(), FileKind::secret);
    if (!bytes) {
        return common::Result<std::shared_ptr<const common::SecureString>>::failure(bytes.error());
    }
    while (!bytes->empty() && (bytes->back() == '\n' || bytes->back() == '\r')) {
        bytes->pop_back();
    }
    if (bytes->empty()) {
        common::secure_erase_memory(bytes->data(), bytes->capacity());
        return common::Result<std::shared_ptr<const common::SecureString>>::failure(
            common::ErrorCode::invalid_argument, "client PSK file contains an empty secret");
    }
    common::SecureString secret{{bytes->data(), bytes->size()}};
    common::secure_erase_memory(bytes->data(), bytes->capacity());
    return std::make_shared<const common::SecureString>(std::move(secret));
}

[[nodiscard]] common::Result<std::string> policy_fingerprint(const ClientPolicy& policy) {
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (raw == nullptr) {
        return common::Result<std::string>::failure(common::ErrorCode::internal_error,
                                                   "failed to allocate policy digest context");
    }
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{raw, &EVP_MD_CTX_free};
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return common::Result<std::string>::failure(common::ErrorCode::internal_error,
                                                   "failed to initialize policy digest");
    }
    const auto update = [&context](const std::string_view value) {
        return EVP_DigestUpdate(context.get(), value.data(), value.size()) == 1;
    };
    const auto update_number = [&update](const std::size_t value) {
        const std::string text = std::to_string(value);
        return update(text) && update("\0");
    };
    bool ok = update(policy.client_id) && update("\0") &&
              update(policy.enabled ? "1" : "0") && update("\0") &&
              update_number(policy.max_tunnels) && update_number(policy.max_connections) &&
              update_number(policy.max_idle_workers) &&
              update(std::to_string(static_cast<std::uint8_t>(policy.certificate.kind))) &&
              update("\0") && update(policy.certificate.value) && update("\0");
    for (const auto& range : policy.allowed_ports) {
        ok = ok && update(range.to_string()) && update("\0");
    }
    if (policy.psk != nullptr) {
        ok = ok && update(policy.psk->view());
    } else {
        ok = false;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0U;
    if (!ok || EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != 32U) {
        return common::Result<std::string>::failure(common::ErrorCode::internal_error,
                                                   "failed to compute policy digest");
    }
    std::string output;
    output.reserve(kSha256HexBytes);
    for (unsigned int index = 0U; index < size; ++index) {
        output.push_back(kHexDigits[digest[index] >> 4U]);
        output.push_back(kHexDigits[digest[index] & 0x0fU]);
    }
    common::secure_erase_memory(digest.data(), digest.size());
    return output;
}

[[nodiscard]] common::Result<std::shared_ptr<const ClientPolicySnapshot>>
load_snapshot(const std::string& config_path, const ClientPolicyLimits& limits);

} // namespace

class ClientPolicySnapshot::Impl final {
  public:
    std::unordered_map<std::string, std::shared_ptr<const ClientPolicy>> policies;
};

class ClientPolicyStore::AtomicSnapshot final {
  public:
    explicit AtomicSnapshot(std::shared_ptr<const ClientPolicySnapshot> value) noexcept
        : value_(std::move(value)) {}

    [[nodiscard]] std::shared_ptr<const ClientPolicySnapshot> load() const noexcept {
        const std::scoped_lock lock{mutex_};
        return value_;
    }

    void store(std::shared_ptr<const ClientPolicySnapshot> value) noexcept {
        const std::scoped_lock lock{mutex_};
        value_ = std::move(value);
    }

  private:
    mutable std::mutex mutex_;
    std::shared_ptr<const ClientPolicySnapshot> value_;
};

namespace {

[[nodiscard]] common::Result<std::shared_ptr<const ClientPolicySnapshot>>
load_snapshot(const std::string& config_path, const ClientPolicyLimits& limits) {
    if (limits.max_clients == 0U || limits.max_clients > 100'000U ||
        limits.max_tunnels_per_client == 0U || limits.max_tunnels_per_client > 4'096U ||
        limits.max_connections_per_client == 0U ||
        limits.max_connections_per_client > 100'000U ||
        limits.max_idle_workers_per_client == 0U ||
        limits.max_idle_workers_per_client > 4'096U) {
        return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
            common::ErrorCode::invalid_argument, "client policy server limits are invalid");
    }
    auto bytes = read_secure_file(config_path, FileKind::policy);
    if (!bytes) {
        return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(bytes.error());
    }
    auto document = parse_policy_json(*bytes);
    if (!document) {
        return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(document.error());
    }
    static const std::set<std::string_view> top_fields{"format_version", "clients"};
    if (!has_exact_fields(*document, top_fields) || document->size() != top_fields.size() ||
        !document->at("format_version").is_number_unsigned() ||
        document->at("format_version").get<std::uint64_t>() != 1U ||
        !document->at("clients").is_array()) {
        return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
            common::ErrorCode::invalid_argument,
            "client policy requires only format_version 1 and a clients array");
    }
    const auto& clients = document->at("clients");
    if (clients.empty() || clients.size() > limits.max_clients) {
        return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
            common::ErrorCode::invalid_argument, "client policy client count is outside the limit");
    }

    static const std::set<std::string_view> client_fields{
        "client_id",          "enabled",        "psk_file",          "allowed_ports",
        "max_tunnels",       "max_connections", "max_idle_workers", "certificate_sha256",
        "certificate_san",
    };
    auto implementation = std::make_shared<ClientPolicySnapshot::Impl>();
    const std::filesystem::path config_directory =
        std::filesystem::path{config_path}.parent_path();
    for (const auto& entry : clients) {
        if (!has_exact_fields(entry, client_fields) ||
            !entry.contains("client_id") || !entry.contains("enabled") ||
            !entry.contains("psk_file") || !entry.contains("allowed_ports") ||
            !entry.contains("max_tunnels") || !entry.contains("max_connections") ||
            !entry.contains("max_idle_workers") || !entry.at("client_id").is_string() ||
            !entry.at("enabled").is_boolean() || !entry.at("psk_file").is_string() ||
            !entry.at("allowed_ports").is_array()) {
            return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
                common::ErrorCode::invalid_argument,
                "each client policy must contain exactly typed identity, PSK, ACL, and quotas");
        }
        auto parsed_id = common::Id::parse(entry.at("client_id").get_ref<const std::string&>(),
                                           common::IdKind::client);
        if (!parsed_id) {
            return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
                common::ErrorCode::invalid_argument, "client policy contains an invalid client_id");
        }
        ClientPolicy policy;
        policy.client_id = parsed_id->str();
        policy.enabled = entry.at("enabled").get<bool>();
        auto psk = load_psk(config_directory, entry.at("psk_file").get_ref<const std::string&>());
        if (!psk) {
            return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(psk.error());
        }
        policy.psk = std::move(*psk);
        const auto& allowed_ports = entry.at("allowed_ports");
        if (allowed_ports.empty() || allowed_ports.size() > 256U) {
            return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
                common::ErrorCode::invalid_argument,
                "allowed_ports must contain between 1 and 256 ranges");
        }
        for (const auto& range_value : allowed_ports) {
            if (!range_value.is_string()) {
                return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
                    common::ErrorCode::invalid_argument, "allowed_ports entries must be strings");
            }
            auto range = common::PortRange::parse(range_value.get_ref<const std::string&>());
            if (!range) {
                return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
                    common::ErrorCode::invalid_argument,
                    "client policy contains an invalid allowed port range");
            }
            policy.allowed_ports.push_back(*range);
        }
        std::sort(policy.allowed_ports.begin(), policy.allowed_ports.end(),
                  [](const auto& left, const auto& right) { return left.start() < right.start(); });
        for (std::size_t index = 1U; index < policy.allowed_ports.size(); ++index) {
            if (policy.allowed_ports[index - 1U].end() >= policy.allowed_ports[index].start()) {
                return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
                    common::ErrorCode::invalid_argument,
                    "client policy allowed port ranges overlap or repeat");
            }
        }
        auto max_tunnels = parse_limit(entry.at("max_tunnels"), limits.max_tunnels_per_client,
                                       "max_tunnels");
        auto max_connections =
            parse_limit(entry.at("max_connections"), limits.max_connections_per_client,
                        "max_connections");
        auto max_idle_workers =
            parse_limit(entry.at("max_idle_workers"), limits.max_idle_workers_per_client,
                        "max_idle_workers");
        auto certificate = parse_certificate_binding(entry);
        if (!max_tunnels || !max_connections || !max_idle_workers || !certificate) {
            const auto* error = !max_tunnels       ? &max_tunnels.error()
                                : !max_connections ? &max_connections.error()
                                : !max_idle_workers ? &max_idle_workers.error()
                                                    : &certificate.error();
            return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(*error);
        }
        policy.max_tunnels = *max_tunnels;
        policy.max_connections = *max_connections;
        policy.max_idle_workers = *max_idle_workers;
        policy.certificate = std::move(*certificate);
        auto fingerprint = policy_fingerprint(policy);
        if (!fingerprint) {
            return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
                fingerprint.error());
        }
        policy.revision_fingerprint = std::move(*fingerprint);
        auto owned = std::make_shared<const ClientPolicy>(std::move(policy));
        if (!implementation->policies.emplace(owned->client_id, std::move(owned)).second) {
            return common::Result<std::shared_ptr<const ClientPolicySnapshot>>::failure(
                common::ErrorCode::already_exists,
                "client policy contains a duplicate client_id");
        }
    }
    return std::shared_ptr<const ClientPolicySnapshot>{
        new ClientPolicySnapshot{std::move(implementation)}};
}

} // namespace

bool ClientPolicy::allows_port(const std::uint16_t port) const noexcept {
    return std::any_of(allowed_ports.begin(), allowed_ports.end(),
                       [port](const common::PortRange& range) { return range.contains(port); });
}

ClientPolicySnapshot::ClientPolicySnapshot(std::shared_ptr<const Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

std::shared_ptr<const ClientPolicy>
ClientPolicySnapshot::find(const std::string_view client_id) const {
    if (implementation_ == nullptr) {
        return {};
    }
    const auto iterator = implementation_->policies.find(std::string{client_id});
    return iterator == implementation_->policies.end() ? nullptr : iterator->second;
}

std::size_t ClientPolicySnapshot::size() const noexcept {
    return implementation_ == nullptr ? 0U : implementation_->policies.size();
}

bool ClientPolicySnapshot::has_certificate_bindings() const noexcept {
    if (implementation_ == nullptr) {
        return false;
    }
    return std::any_of(implementation_->policies.begin(), implementation_->policies.end(),
                       [](const auto& entry) {
                           return entry.second->certificate.kind !=
                                  ClientCertificateBindingKind::none;
                       });
}

common::Result<std::shared_ptr<ClientPolicyStore>>
ClientPolicyStore::open(std::string config_path, const ClientPolicyLimits limits) {
    auto loaded = load_snapshot(config_path, limits);
    if (!loaded) {
        return common::Result<std::shared_ptr<ClientPolicyStore>>::failure(loaded.error());
    }
    return std::shared_ptr<ClientPolicyStore>{
        new ClientPolicyStore{std::move(config_path), limits, std::move(*loaded)}};
}

ClientPolicyStore::ClientPolicyStore(std::string config_path, const ClientPolicyLimits limits,
                                     std::shared_ptr<const ClientPolicySnapshot> snapshot) noexcept
    : config_path_(std::move(config_path)), limits_(limits),
      snapshot_(std::make_unique<AtomicSnapshot>(std::move(snapshot))) {}

common::Result<std::vector<std::string>>
ClientPolicyStore::reload(const SnapshotValidator& validator) {
    auto replacement = load_snapshot(config_path_, limits_);
    if (!replacement) {
        return common::Result<std::vector<std::string>>::failure(replacement.error());
    }
    if (validator) {
        auto validated = validator(**replacement);
        if (!validated) {
            return common::Result<std::vector<std::string>>::failure(validated.error());
        }
    }
    const auto previous = snapshot();
    std::set<std::string, std::less<>> changed;
    if (previous != nullptr && previous->implementation_ != nullptr) {
        for (const auto& [client_id, policy] : previous->implementation_->policies) {
            const auto current = (*replacement)->find(client_id);
            if (current == nullptr ||
                current->revision_fingerprint != policy->revision_fingerprint) {
                changed.emplace(client_id);
            }
        }
    }
    if ((*replacement)->implementation_ != nullptr) {
        for (const auto& [client_id, policy] : (*replacement)->implementation_->policies) {
            static_cast<void>(policy);
            if (previous == nullptr || previous->find(client_id) == nullptr) {
                changed.emplace(client_id);
            }
        }
    }
    snapshot_->store(std::move(*replacement));
    return std::vector<std::string>{changed.begin(), changed.end()};
}

std::shared_ptr<const ClientPolicySnapshot> ClientPolicyStore::snapshot() const noexcept {
    return snapshot_->load();
}

std::shared_ptr<const ClientPolicy> ClientPolicyStore::find(const std::string_view client_id) const {
    const auto current = snapshot();
    return current == nullptr ? nullptr : current->find(client_id);
}

const std::string& ClientPolicyStore::config_path() const noexcept { return config_path_; }

} // namespace minitun::server
