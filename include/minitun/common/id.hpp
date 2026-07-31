#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include <minitun/common/result.hpp>

namespace minitun::common {

/// Identifies the namespace and stable textual prefix of a MiniTun ID.
enum class IdKind : std::uint8_t {
    server,
    tunnel,
    request,
    client,
    connection,
};

/// Number of cryptographically random bytes encoded into every ID.
///
/// Sixteen bytes provide 128 bits of entropy, exceeding the project's
/// minimum requirement of 96 bits.
inline constexpr std::size_t kIdRandomBytes = 16;
inline constexpr std::size_t kIdHexCharacters = kIdRandomBytes * 2;

[[nodiscard]] std::string_view to_string(IdKind kind) noexcept;

/// Returns the complete prefix, including its trailing underscore.
///
/// An invalid enum value returns an empty view.
[[nodiscard]] std::string_view id_prefix(IdKind kind) noexcept;

/// A validated, type-aware MiniTun identifier.
///
/// The canonical form is "<prefix><32 lowercase hexadecimal characters>".
/// Instances can only be created by generate() or parse(), so a constructed
/// Id always satisfies this invariant.
class Id final {
  public:
    /// Generates an ID using OpenSSL RAND_bytes.
    [[nodiscard]] static Result<Id> generate(IdKind kind);

    /// Parses an ID and infers its kind from the canonical prefix.
    [[nodiscard]] static Result<Id> parse(std::string_view text);

    /// Parses an ID and additionally requires the expected kind.
    [[nodiscard]] static Result<Id> parse(std::string_view text, IdKind expected_kind);

    [[nodiscard]] IdKind kind() const noexcept;
    [[nodiscard]] const std::string& str() const noexcept;
    [[nodiscard]] std::string_view suffix() const noexcept;

    friend bool operator==(const Id&, const Id&) = default;
    friend auto operator<=>(const Id&, const Id&) = default;

  private:
    Id(IdKind kind, std::string text);

    IdKind kind_;
    std::string text_;
};

std::ostream& operator<<(std::ostream& stream, const Id& id);

} // namespace minitun::common
