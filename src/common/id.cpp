#include <minitun/common/id.hpp>

#include <array>
#include <ostream>
#include <utility>

#include <openssl/rand.h>

namespace minitun::common {
namespace {

struct IdKindInfo final {
    IdKind kind;
    std::string_view name;
    std::string_view prefix;
};

constexpr std::array kIdKinds{
    IdKindInfo{IdKind::server, "server", "srv_"},
    IdKindInfo{IdKind::tunnel, "tunnel", "tun_"},
    IdKindInfo{IdKind::request, "request", "req_"},
    IdKindInfo{IdKind::client, "client", "client_"},
    IdKindInfo{IdKind::connection, "connection", "conn_"},
};

constexpr std::string_view kHexDigits = "0123456789abcdef";

[[nodiscard]] const IdKindInfo* find_kind(const IdKind kind) noexcept {
    for (const auto& info : kIdKinds) {
        if (info.kind == kind) {
            return &info;
        }
    }
    return nullptr;
}

[[nodiscard]] const IdKindInfo* find_kind(const std::string_view text) noexcept {
    for (const auto& info : kIdKinds) {
        if (text.starts_with(info.prefix)) {
            return &info;
        }
    }
    return nullptr;
}

[[nodiscard]] bool is_lowercase_hex(const char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

} // namespace

static_assert(kIdRandomBytes * 8 >= 96, "MiniTun IDs require at least 96 random bits");

std::string_view to_string(const IdKind kind) noexcept {
    const auto* const info = find_kind(kind);
    return info == nullptr ? std::string_view{"unknown"} : info->name;
}

std::string_view id_prefix(const IdKind kind) noexcept {
    const auto* const info = find_kind(kind);
    return info == nullptr ? std::string_view{} : info->prefix;
}

Result<Id> Id::generate(const IdKind kind) {
    const std::string_view prefix = id_prefix(kind);
    if (prefix.empty()) {
        return Result<Id>::failure(ErrorCode::invalid_argument, "unknown ID kind");
    }

    std::array<unsigned char, kIdRandomBytes> random_bytes{};
    if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
        return Result<Id>::failure(ErrorCode::internal_error,
                                   "cryptographically secure ID generation failed");
    }

    std::string text;
    text.reserve(prefix.size() + kIdHexCharacters);
    text.append(prefix);
    for (const unsigned char byte : random_bytes) {
        text.push_back(kHexDigits[byte >> 4U]);
        text.push_back(kHexDigits[byte & 0x0fU]);
    }

    return Id{kind, std::move(text)};
}

Result<Id> Id::parse(const std::string_view text) {
    if (text.empty()) {
        return Result<Id>::failure(ErrorCode::invalid_argument, "ID must not be empty");
    }

    const auto* const info = find_kind(text);
    if (info == nullptr) {
        return Result<Id>::failure(ErrorCode::invalid_argument, "ID has an unknown prefix");
    }

    const std::string_view encoded_random = text.substr(info->prefix.size());
    if (encoded_random.size() != kIdHexCharacters) {
        return Result<Id>::failure(
            ErrorCode::invalid_argument,
            "ID must have exactly 32 lowercase hexadecimal characters after its prefix");
    }

    for (const char character : encoded_random) {
        if (!is_lowercase_hex(character)) {
            return Result<Id>::failure(ErrorCode::invalid_argument,
                                       "ID contains a non-lowercase-hexadecimal character");
        }
    }

    return Id{info->kind, std::string{text}};
}

Result<Id> Id::parse(const std::string_view text, const IdKind expected_kind) {
    if (id_prefix(expected_kind).empty()) {
        return Result<Id>::failure(ErrorCode::invalid_argument, "unknown expected ID kind");
    }

    auto parsed = parse(text);
    if (!parsed) {
        return parsed;
    }
    if (parsed->kind() != expected_kind) {
        return Result<Id>::failure(ErrorCode::invalid_argument,
                                   "ID kind does not match the expected kind");
    }
    return parsed;
}

IdKind Id::kind() const noexcept { return kind_; }

const std::string& Id::str() const noexcept { return text_; }

std::string_view Id::suffix() const noexcept {
    return std::string_view{text_}.substr(id_prefix(kind_).size());
}

Id::Id(const IdKind kind, std::string text) : kind_(kind), text_(std::move(text)) {}

std::ostream& operator<<(std::ostream& stream, const Id& id) {
    stream << id.str();
    return stream;
}

} // namespace minitun::common
