#include <minitun/ipc/frame.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include <minitun/common/secure_string.hpp>

namespace minitun::ipc {
namespace {

[[nodiscard]] std::size_t effective_limit(const std::size_t requested_limit) noexcept {
    return std::min(requested_limit, kDefaultMaxFrameSize);
}

[[nodiscard]] common::Error decoder_error(const common::ErrorCode code) {
    switch (code) {
    case common::ErrorCode::frame_too_large:
        return common::Error{code, "IPC frame exceeds the configured size limit"};
    case common::ErrorCode::resource_exhausted:
        return common::Error{code, "insufficient memory while decoding an IPC frame"};
    default:
        return common::Error{common::ErrorCode::protocol_error,
                             "IPC frame decoder is in a failed state"};
    }
}

[[nodiscard]] std::uint32_t decode_length(const std::uint8_t* const header) noexcept {
    return (static_cast<std::uint32_t>(header[0]) << 24U) |
           (static_cast<std::uint32_t>(header[1]) << 16U) |
           (static_cast<std::uint32_t>(header[2]) << 8U) | static_cast<std::uint32_t>(header[3]);
}

} // namespace

common::Result<std::vector<std::uint8_t>> encode_frame(const std::string_view payload,
                                                       const std::size_t max_frame_size) {
    if (payload.empty()) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::protocol_error, "IPC frame payload must not be empty");
    }
    const std::size_t limit = effective_limit(max_frame_size);
    if (payload.size() > limit || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::frame_too_large, "IPC frame exceeds the configured size limit");
    }

    try {
        std::vector<std::uint8_t> frame(kFrameHeaderSize + payload.size());
        const auto length = static_cast<std::uint32_t>(payload.size());
        frame[0] = static_cast<std::uint8_t>((length >> 24U) & 0xffU);
        frame[1] = static_cast<std::uint8_t>((length >> 16U) & 0xffU);
        frame[2] = static_cast<std::uint8_t>((length >> 8U) & 0xffU);
        frame[3] = static_cast<std::uint8_t>(length & 0xffU);
        std::transform(payload.begin(), payload.end(), frame.begin() + kFrameHeaderSize,
                       [](const char byte) {
                           return static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
                       });
        return frame;
    } catch (const std::bad_alloc&) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while encoding an IPC frame");
    }
}

FrameDecoder::FrameDecoder(const std::size_t max_frame_size)
    : max_frame_size_(effective_limit(max_frame_size)) {}

FrameDecoder::~FrameDecoder() noexcept {
    common::secure_erase_memory(payload_.data(), payload_.size());
}

common::Result<std::vector<std::string>>
FrameDecoder::feed(const std::span<const std::uint8_t> bytes) {
    if (failed_) {
        return common::Result<std::vector<std::string>>::failure(decoder_error(failure_code_));
    }

    try {
        std::vector<std::string> frames;
        std::size_t offset = 0;

        while (offset < bytes.size()) {
            if (!reading_payload_) {
                const std::size_t available = bytes.size() - offset;
                const std::size_t needed = kFrameHeaderSize - header_size_;
                const std::size_t count = std::min(available, needed);
                std::copy_n(bytes.data() + offset, count, header_ + header_size_);
                header_size_ += count;
                offset += count;

                if (header_size_ != kFrameHeaderSize) {
                    continue;
                }

                expected_payload_size_ = decode_length(header_);
                header_size_ = 0;
                if (expected_payload_size_ > max_frame_size_) {
                    fail(common::ErrorCode::frame_too_large);
                    return common::Result<std::vector<std::string>>::failure(
                        decoder_error(failure_code_));
                }

                if (expected_payload_size_ == 0U) {
                    fail(common::ErrorCode::protocol_error);
                    return common::Result<std::vector<std::string>>::failure(
                        common::ErrorCode::protocol_error, "IPC frame payload must not be empty");
                }

                payload_.clear();
                payload_.reserve(expected_payload_size_);
                reading_payload_ = true;
            }

            const std::size_t expected = expected_payload_size_;
            const std::size_t needed = expected - payload_.size();
            const std::size_t available = bytes.size() - offset;
            const std::size_t count = std::min(available, needed);
            payload_.append(reinterpret_cast<const char*>(bytes.data() + offset), count);
            offset += count;

            if (payload_.size() == expected) {
                frames.emplace_back(payload_);
                common::secure_erase_memory(payload_.data(), payload_.size());
                payload_.clear();
                expected_payload_size_ = 0;
                reading_payload_ = false;
            }
        }

        return frames;
    } catch (const std::bad_alloc&) {
        fail(common::ErrorCode::resource_exhausted);
        return common::Result<std::vector<std::string>>::failure(decoder_error(failure_code_));
    }
}

common::Result<void> FrameDecoder::finish() const {
    if (failed_) {
        return common::Result<void>::failure(decoder_error(failure_code_));
    }
    if (header_size_ != 0U || reading_payload_) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "IPC stream ended in the middle of a frame");
    }
    return common::Result<void>::success();
}

void FrameDecoder::reset() noexcept {
    common::secure_erase_memory(payload_.data(), payload_.size());
    expected_payload_size_ = 0;
    header_size_ = 0;
    payload_.clear();
    reading_payload_ = false;
    failed_ = false;
    failure_code_ = common::ErrorCode::protocol_error;
}

std::size_t FrameDecoder::buffered_size() const noexcept { return header_size_ + payload_.size(); }

std::size_t FrameDecoder::max_frame_size() const noexcept { return max_frame_size_; }

void FrameDecoder::fail(const common::ErrorCode code) noexcept {
    common::secure_erase_memory(payload_.data(), payload_.size());
    expected_payload_size_ = 0;
    header_size_ = 0;
    payload_.clear();
    reading_payload_ = false;
    failed_ = true;
    failure_code_ = code;
}

} // namespace minitun::ipc
