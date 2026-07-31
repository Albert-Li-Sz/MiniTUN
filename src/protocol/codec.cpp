#include <minitun/protocol/codec.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace minitun::protocol {
namespace {

[[nodiscard]] common::Error truncated_payload_error() {
    return common::Error{common::ErrorCode::protocol_error,
                         "remote message payload ended before a field was complete"};
}

} // namespace

PayloadWriter::PayloadWriter(const std::size_t max_payload_size) noexcept
    : max_payload_size_(std::min(max_payload_size, kMaxPayloadSize)) {}

common::Result<void> PayloadWriter::write_u8(const std::uint8_t value) {
    const std::array bytes{value};
    return append(bytes);
}

common::Result<void> PayloadWriter::write_u16(const std::uint16_t value) {
    const std::array<std::uint8_t, 2U> bytes{
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>(value & 0xffU),
    };
    return append(bytes);
}

common::Result<void> PayloadWriter::write_u32(const std::uint32_t value) {
    const std::array<std::uint8_t, 4U> bytes{
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>(value & 0xffU),
    };
    return append(bytes);
}

common::Result<void> PayloadWriter::write_u64(const std::uint64_t value) {
    const std::array<std::uint8_t, 8U> bytes{
        static_cast<std::uint8_t>((value >> 56U) & 0xffU),
        static_cast<std::uint8_t>((value >> 48U) & 0xffU),
        static_cast<std::uint8_t>((value >> 40U) & 0xffU),
        static_cast<std::uint8_t>((value >> 32U) & 0xffU),
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>(value & 0xffU),
    };
    return append(bytes);
}

common::Result<void> PayloadWriter::write_bytes(const std::span<const std::uint8_t> value) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
        return fail(common::ErrorCode::invalid_argument,
                    "remote message byte field exceeds the protocol limit");
    }
    const auto length_result = write_u16(static_cast<std::uint16_t>(value.size()));
    if (!length_result) {
        return length_result;
    }
    return append(value);
}

common::Result<void> PayloadWriter::write_string(const std::string_view value) {
    if (!is_valid_utf8(value) || value.find('\0') != std::string_view::npos) {
        return fail(common::ErrorCode::invalid_argument,
                    "remote message string field is not valid UTF-8 text");
    }
    const auto* const data = reinterpret_cast<const std::uint8_t*>(value.data());
    return write_bytes({data, value.size()});
}

common::Result<std::vector<std::uint8_t>> PayloadWriter::finish() && {
    if (failure_.has_value()) {
        return common::Result<std::vector<std::uint8_t>>::failure(std::move(*failure_));
    }
    return std::move(bytes_);
}

std::size_t PayloadWriter::size() const noexcept { return bytes_.size(); }

common::Result<void> PayloadWriter::append(const std::span<const std::uint8_t> value) {
    if (failure_.has_value()) {
        return common::Result<void>::failure(*failure_);
    }
    if (value.size() > max_payload_size_ - std::min(bytes_.size(), max_payload_size_)) {
        return fail(common::ErrorCode::frame_too_large,
                    "remote message payload exceeds the configured size limit");
    }

    try {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return common::Result<void>::success();
    } catch (const std::bad_alloc&) {
        return fail(common::ErrorCode::resource_exhausted,
                    "insufficient memory while encoding a remote message");
    } catch (const std::length_error&) {
        return fail(common::ErrorCode::resource_exhausted,
                    "insufficient memory while encoding a remote message");
    }
}

common::Result<void> PayloadWriter::fail(const common::ErrorCode code, std::string message) {
    if (!failure_.has_value()) {
        failure_.emplace(code, std::move(message));
    }
    return common::Result<void>::failure(*failure_);
}

PayloadReader::PayloadReader(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

common::Result<std::uint8_t> PayloadReader::read_u8() {
    if (remaining() < 1U) {
        return common::Result<std::uint8_t>::failure(truncated_payload_error());
    }
    return bytes_[offset_++];
}

common::Result<std::uint16_t> PayloadReader::read_u16() {
    if (remaining() < 2U) {
        return common::Result<std::uint16_t>::failure(truncated_payload_error());
    }
    const std::uint16_t value =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes_[offset_]) << 8U) |
                                   static_cast<std::uint16_t>(bytes_[offset_ + 1U]));
    offset_ += 2U;
    return value;
}

common::Result<std::uint32_t> PayloadReader::read_u32() {
    if (remaining() < 4U) {
        return common::Result<std::uint32_t>::failure(truncated_payload_error());
    }
    const std::uint32_t value = (static_cast<std::uint32_t>(bytes_[offset_]) << 24U) |
                                (static_cast<std::uint32_t>(bytes_[offset_ + 1U]) << 16U) |
                                (static_cast<std::uint32_t>(bytes_[offset_ + 2U]) << 8U) |
                                static_cast<std::uint32_t>(bytes_[offset_ + 3U]);
    offset_ += 4U;
    return value;
}

common::Result<std::uint64_t> PayloadReader::read_u64() {
    if (remaining() < 8U) {
        return common::Result<std::uint64_t>::failure(truncated_payload_error());
    }
    const std::uint64_t value = (static_cast<std::uint64_t>(bytes_[offset_]) << 56U) |
                                (static_cast<std::uint64_t>(bytes_[offset_ + 1U]) << 48U) |
                                (static_cast<std::uint64_t>(bytes_[offset_ + 2U]) << 40U) |
                                (static_cast<std::uint64_t>(bytes_[offset_ + 3U]) << 32U) |
                                (static_cast<std::uint64_t>(bytes_[offset_ + 4U]) << 24U) |
                                (static_cast<std::uint64_t>(bytes_[offset_ + 5U]) << 16U) |
                                (static_cast<std::uint64_t>(bytes_[offset_ + 6U]) << 8U) |
                                static_cast<std::uint64_t>(bytes_[offset_ + 7U]);
    offset_ += 8U;
    return value;
}

common::Result<std::vector<std::uint8_t>>
PayloadReader::read_bytes(const std::size_t max_length) {
    if (remaining() < 2U) {
        return common::Result<std::vector<std::uint8_t>>::failure(truncated_payload_error());
    }
    const std::size_t length =
        (static_cast<std::size_t>(bytes_[offset_]) << 8U) |
        static_cast<std::size_t>(bytes_[offset_ + 1U]);
    if (length > max_length) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::protocol_error,
            "remote message byte field exceeds the permitted length");
    }
    if (remaining() - 2U < length) {
        return common::Result<std::vector<std::uint8_t>>::failure(truncated_payload_error());
    }

    try {
        const std::size_t begin = offset_ + 2U;
        std::vector<std::uint8_t> value(
            bytes_.begin() + static_cast<std::ptrdiff_t>(begin),
            bytes_.begin() + static_cast<std::ptrdiff_t>(begin + length));
        offset_ = begin + length;
        return value;
    } catch (const std::bad_alloc&) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while decoding a remote message");
    } catch (const std::length_error&) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while decoding a remote message");
    }
}

common::Result<std::string> PayloadReader::read_string(const std::size_t max_length) {
    auto value = read_bytes(max_length);
    if (!value) {
        return common::Result<std::string>::failure(value.error());
    }

    try {
        std::string text(value->begin(), value->end());
        if (!is_valid_utf8(text) || text.find('\0') != std::string::npos) {
            return common::Result<std::string>::failure(
                common::ErrorCode::protocol_error,
                "remote message string field is not valid UTF-8 text");
        }
        return text;
    } catch (const std::bad_alloc&) {
        return common::Result<std::string>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while decoding a remote message");
    } catch (const std::length_error&) {
        return common::Result<std::string>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while decoding a remote message");
    }
}

common::Result<void> PayloadReader::require_end() const {
    if (remaining() != 0U) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote message payload contains trailing bytes");
    }
    return common::Result<void>::success();
}

std::size_t PayloadReader::remaining() const noexcept { return bytes_.size() - offset_; }

bool is_valid_utf8(const std::string_view value) noexcept {
    const auto* const bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0U;
    while (index < value.size()) {
        const unsigned char first = bytes[index];
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        std::uint32_t minimum = 0U;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1U;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2U;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }

        if (index + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const unsigned char continuation = bytes[index + offset];
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

} // namespace minitun::protocol
