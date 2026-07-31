#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace minitun::common {

void secure_erase_memory(void* data, std::size_t size) noexcept;

/// Move-only storage for short-lived secrets.
///
/// The value is held in an exclusively owned, fixed-size buffer. This type
/// intentionally provides no copying operation or API that creates an owning
/// std::string. The returned views remain valid only until the object is
/// cleared, moved from, or destroyed.
class SecureString final {
  public:
    SecureString() noexcept = default;
    explicit SecureString(std::string_view value);
    ~SecureString() noexcept;

    SecureString(const SecureString&) = delete;
    SecureString& operator=(const SecureString&) = delete;

    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(SecureString&& other) noexcept;

    [[nodiscard]] std::string_view view() const noexcept;
    /// Returns the fixed buffer, or nullptr when the value is empty.
    [[nodiscard]] const char* data() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    /// Erases the complete allocation before releasing it.
    void clear() noexcept;

    /// Compares equally sized values without content-dependent early exits.
    ///
    /// Length is not secret and a length mismatch returns immediately.
    [[nodiscard]] bool equals(const SecureString& other) const noexcept;

  private:
    std::unique_ptr<char[]> buffer_;
    std::size_t size_{0};
};

} // namespace minitun::common
