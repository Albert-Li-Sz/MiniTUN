#include <minitun/common/secure_string.hpp>

#include <cstring>
#include <utility>

#include <openssl/crypto.h>

namespace minitun::common {

void secure_erase_memory(void* const data, const std::size_t size) noexcept {
    if (data != nullptr && size != 0U) {
        OPENSSL_cleanse(data, size);
    }
}

SecureString::SecureString(const std::string_view value) : size_(value.size()) {
    if (value.empty()) {
        return;
    }

    buffer_ = std::make_unique<char[]>(size_);
    std::memcpy(buffer_.get(), value.data(), size_);
}

SecureString::~SecureString() noexcept { clear(); }

SecureString::SecureString(SecureString&& other) noexcept
    : buffer_(std::move(other.buffer_)), size_(std::exchange(other.size_, 0U)) {}

SecureString& SecureString::operator=(SecureString&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    clear();
    buffer_ = std::move(other.buffer_);
    size_ = std::exchange(other.size_, 0U);
    return *this;
}

std::string_view SecureString::view() const noexcept {
    if (empty()) {
        return {};
    }
    return {buffer_.get(), size_};
}

const char* SecureString::data() const noexcept { return buffer_.get(); }

std::size_t SecureString::size() const noexcept { return size_; }

bool SecureString::empty() const noexcept { return size_ == 0U; }

void SecureString::clear() noexcept {
    if (buffer_ != nullptr) {
        secure_erase_memory(buffer_.get(), size_);
        buffer_.reset();
    }
    size_ = 0U;
}

bool SecureString::equals(const SecureString& other) const noexcept {
    if (size_ != other.size_) {
        return false;
    }
    if (empty()) {
        return true;
    }
    return CRYPTO_memcmp(buffer_.get(), other.buffer_.get(), size_) == 0;
}

} // namespace minitun::common
