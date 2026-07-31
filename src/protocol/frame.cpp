#include <minitun/protocol/frame.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <minitun/common/secure_string.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::size_t effective_frame_limit(const std::size_t requested) noexcept {
    return std::clamp(requested, kFrameHeaderSize, kMaxFrameSize);
}

void write_u16(std::uint8_t* const output, const std::uint16_t value) noexcept {
    output[0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    output[1] = static_cast<std::uint8_t>(value & 0xffU);
}

void write_u32(std::uint8_t* const output, const std::uint32_t value) noexcept {
    output[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    output[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    output[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    output[3] = static_cast<std::uint8_t>(value & 0xffU);
}

void write_u64(std::uint8_t* const output, const std::uint64_t value) noexcept {
    output[0] = static_cast<std::uint8_t>((value >> 56U) & 0xffU);
    output[1] = static_cast<std::uint8_t>((value >> 48U) & 0xffU);
    output[2] = static_cast<std::uint8_t>((value >> 40U) & 0xffU);
    output[3] = static_cast<std::uint8_t>((value >> 32U) & 0xffU);
    output[4] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    output[5] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    output[6] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    output[7] = static_cast<std::uint8_t>(value & 0xffU);
}

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* const input) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[0]) << 8U) |
                                      static_cast<std::uint16_t>(input[1]));
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const input) noexcept {
    return (static_cast<std::uint32_t>(input[0]) << 24U) |
           (static_cast<std::uint32_t>(input[1]) << 16U) |
           (static_cast<std::uint32_t>(input[2]) << 8U) | static_cast<std::uint32_t>(input[3]);
}

[[nodiscard]] std::uint64_t read_u64(const std::uint8_t* const input) noexcept {
    return (static_cast<std::uint64_t>(input[0]) << 56U) |
           (static_cast<std::uint64_t>(input[1]) << 48U) |
           (static_cast<std::uint64_t>(input[2]) << 40U) |
           (static_cast<std::uint64_t>(input[3]) << 32U) |
           (static_cast<std::uint64_t>(input[4]) << 24U) |
           (static_cast<std::uint64_t>(input[5]) << 16U) |
           (static_cast<std::uint64_t>(input[6]) << 8U) | static_cast<std::uint64_t>(input[7]);
}

[[nodiscard]] common::Error decoder_error(const common::ErrorCode code) {
    switch (code) {
    case common::ErrorCode::frame_too_large:
        return common::Error{code, "remote frame exceeds the configured size limit"};
    case common::ErrorCode::unsupported_version:
        return common::Error{code, "remote protocol version is unsupported"};
    case common::ErrorCode::resource_exhausted:
        return common::Error{code, "insufficient memory while decoding a remote frame"};
    default:
        return common::Error{common::ErrorCode::protocol_error,
                             "remote frame decoder is in a failed state"};
    }
}

} // namespace

std::string_view to_string(const MessageType type) noexcept {
    switch (type) {
    case MessageType::hello:
        return "HELLO";
    case MessageType::hello_ack:
        return "HELLO_ACK";
    case MessageType::auth:
        return "AUTH";
    case MessageType::auth_ok:
        return "AUTH_OK";
    case MessageType::auth_error:
        return "AUTH_ERROR";
    case MessageType::register_tunnel:
        return "REGISTER_TUNNEL";
    case MessageType::register_tunnel_ok:
        return "REGISTER_TUNNEL_OK";
    case MessageType::register_tunnel_error:
        return "REGISTER_TUNNEL_ERROR";
    case MessageType::unregister_tunnel:
        return "UNREGISTER_TUNNEL";
    case MessageType::unregister_tunnel_ok:
        return "UNREGISTER_TUNNEL_OK";
    case MessageType::request_workers:
        return "REQUEST_WORKERS";
    case MessageType::ping:
        return "PING";
    case MessageType::pong:
        return "PONG";
    case MessageType::goaway:
        return "GOAWAY";
    case MessageType::error:
        return "ERROR";
    case MessageType::worker_hello:
        return "WORKER_HELLO";
    case MessageType::worker_accepted:
        return "WORKER_ACCEPTED";
    case MessageType::start_relay:
        return "START_RELAY";
    case MessageType::local_connect_ok:
        return "LOCAL_CONNECT_OK";
    case MessageType::local_connect_error:
        return "LOCAL_CONNECT_ERROR";
    }
    return "UNKNOWN";
}

std::optional<MessageType> message_type_from_wire(const std::uint16_t value) noexcept {
    switch (value) {
    case static_cast<std::uint16_t>(MessageType::hello):
        return MessageType::hello;
    case static_cast<std::uint16_t>(MessageType::hello_ack):
        return MessageType::hello_ack;
    case static_cast<std::uint16_t>(MessageType::auth):
        return MessageType::auth;
    case static_cast<std::uint16_t>(MessageType::auth_ok):
        return MessageType::auth_ok;
    case static_cast<std::uint16_t>(MessageType::auth_error):
        return MessageType::auth_error;
    case static_cast<std::uint16_t>(MessageType::register_tunnel):
        return MessageType::register_tunnel;
    case static_cast<std::uint16_t>(MessageType::register_tunnel_ok):
        return MessageType::register_tunnel_ok;
    case static_cast<std::uint16_t>(MessageType::register_tunnel_error):
        return MessageType::register_tunnel_error;
    case static_cast<std::uint16_t>(MessageType::unregister_tunnel):
        return MessageType::unregister_tunnel;
    case static_cast<std::uint16_t>(MessageType::unregister_tunnel_ok):
        return MessageType::unregister_tunnel_ok;
    case static_cast<std::uint16_t>(MessageType::request_workers):
        return MessageType::request_workers;
    case static_cast<std::uint16_t>(MessageType::ping):
        return MessageType::ping;
    case static_cast<std::uint16_t>(MessageType::pong):
        return MessageType::pong;
    case static_cast<std::uint16_t>(MessageType::goaway):
        return MessageType::goaway;
    case static_cast<std::uint16_t>(MessageType::error):
        return MessageType::error;
    case static_cast<std::uint16_t>(MessageType::worker_hello):
        return MessageType::worker_hello;
    case static_cast<std::uint16_t>(MessageType::worker_accepted):
        return MessageType::worker_accepted;
    case static_cast<std::uint16_t>(MessageType::start_relay):
        return MessageType::start_relay;
    case static_cast<std::uint16_t>(MessageType::local_connect_ok):
        return MessageType::local_connect_ok;
    case static_cast<std::uint16_t>(MessageType::local_connect_error):
        return MessageType::local_connect_error;
    default:
        return std::nullopt;
    }
}

bool is_control_message(const MessageType type) noexcept {
    const auto value = static_cast<std::underlying_type_t<MessageType>>(type);
    return value >= static_cast<std::uint16_t>(MessageType::hello) &&
           value <= static_cast<std::uint16_t>(MessageType::error);
}

bool is_worker_message(const MessageType type) noexcept {
    const auto value = static_cast<std::underlying_type_t<MessageType>>(type);
    return value >= static_cast<std::uint16_t>(MessageType::worker_hello) &&
           value <= static_cast<std::uint16_t>(MessageType::local_connect_error);
}

common::Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame,
                                                       const std::size_t max_frame_size) {
    const auto wire_type = static_cast<std::uint16_t>(frame.type);
    if (!message_type_from_wire(wire_type).has_value()) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::protocol_error, "remote frame message type is invalid");
    }
    if (frame.flags != 0U) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::protocol_error, "remote frame contains unsupported flags");
    }

    const std::size_t limit = effective_frame_limit(max_frame_size);
    if (frame.payload.size() > limit - kFrameHeaderSize ||
        frame.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::frame_too_large,
            "remote frame exceeds the configured size limit");
    }

    try {
        std::vector<std::uint8_t> encoded(kFrameHeaderSize + frame.payload.size());
        write_u32(encoded.data(), kFrameMagic);
        write_u16(encoded.data() + 4U, kProtocolVersion);
        write_u16(encoded.data() + 6U, wire_type);
        write_u32(encoded.data() + 8U, frame.flags);
        write_u32(encoded.data() + 12U, static_cast<std::uint32_t>(frame.payload.size()));
        write_u64(encoded.data() + 16U, frame.request_id);
        std::copy(frame.payload.begin(), frame.payload.end(), encoded.begin() + kFrameHeaderSize);
        return encoded;
    } catch (const std::bad_alloc&) {
        return common::Result<std::vector<std::uint8_t>>::failure(
            common::ErrorCode::resource_exhausted,
            "insufficient memory while encoding a remote frame");
    }
}

FrameDecoder::FrameDecoder(const std::size_t max_frame_size) noexcept
    : max_frame_size_(effective_frame_limit(max_frame_size)) {}

FrameDecoder::~FrameDecoder() noexcept {
    common::secure_erase_memory(payload_.data(), payload_.size());
}

common::Result<std::vector<Frame>> FrameDecoder::feed(const std::span<const std::uint8_t> bytes) {
    if (failed_) {
        return common::Result<std::vector<Frame>>::failure(decoder_error(failure_code_));
    }

    try {
        std::vector<Frame> frames;
        std::size_t offset = 0U;

        while (offset < bytes.size()) {
            if (!reading_payload_) {
                const std::size_t available = bytes.size() - offset;
                const std::size_t needed = kFrameHeaderSize - header_size_;
                const std::size_t count = std::min(available, needed);
                std::copy_n(bytes.data() + offset, count, header_.data() + header_size_);
                header_size_ += count;
                offset += count;

                if (header_size_ != kFrameHeaderSize) {
                    continue;
                }

                const auto parsed = parse_header();
                header_size_ = 0U;
                if (!parsed) {
                    return common::Result<std::vector<Frame>>::failure(parsed.error());
                }

                if (expected_payload_size_ == 0U) {
                    frames.push_back(Frame{current_type_, current_flags_, current_request_id_, {}});
                    reset_current_frame();
                    continue;
                }

                payload_.clear();
                payload_.reserve(expected_payload_size_);
                reading_payload_ = true;
            }

            const std::size_t needed =
                static_cast<std::size_t>(expected_payload_size_) - payload_.size();
            const std::size_t available = bytes.size() - offset;
            const std::size_t count = std::min(available, needed);
            payload_.insert(payload_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
            offset += count;

            if (payload_.size() == expected_payload_size_) {
                frames.push_back(Frame{current_type_, current_flags_, current_request_id_,
                                       std::move(payload_)});
                payload_ = {};
                reset_current_frame();
            }
        }

        return frames;
    } catch (const std::bad_alloc&) {
        fail(common::ErrorCode::resource_exhausted);
        return common::Result<std::vector<Frame>>::failure(decoder_error(failure_code_));
    } catch (const std::length_error&) {
        fail(common::ErrorCode::resource_exhausted);
        return common::Result<std::vector<Frame>>::failure(decoder_error(failure_code_));
    }
}

common::Result<void> FrameDecoder::finish() const {
    if (failed_) {
        return common::Result<void>::failure(decoder_error(failure_code_));
    }
    if (header_size_ != 0U || reading_payload_) {
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote stream ended in the middle of a frame");
    }
    return common::Result<void>::success();
}

void FrameDecoder::reset() noexcept {
    common::secure_erase_memory(payload_.data(), payload_.size());
    header_.fill(0U);
    header_size_ = 0U;
    failed_ = false;
    failure_code_ = common::ErrorCode::protocol_error;
    reset_current_frame();
}

std::size_t FrameDecoder::buffered_size() const noexcept {
    return header_size_ + payload_.size();
}

std::size_t FrameDecoder::max_frame_size() const noexcept { return max_frame_size_; }

common::Result<void> FrameDecoder::parse_header() {
    if (read_u32(header_.data()) != kFrameMagic) {
        fail(common::ErrorCode::protocol_error);
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote frame magic is invalid");
    }
    if (read_u16(header_.data() + 4U) != kProtocolVersion) {
        fail(common::ErrorCode::unsupported_version);
        return common::Result<void>::failure(common::ErrorCode::unsupported_version,
                                             "remote protocol version is unsupported");
    }

    const auto type = message_type_from_wire(read_u16(header_.data() + 6U));
    if (!type.has_value()) {
        fail(common::ErrorCode::protocol_error);
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote frame message type is invalid");
    }

    const std::uint32_t flags = read_u32(header_.data() + 8U);
    if (flags != 0U) {
        fail(common::ErrorCode::protocol_error);
        return common::Result<void>::failure(common::ErrorCode::protocol_error,
                                             "remote frame contains unsupported flags");
    }

    const std::uint32_t payload_size = read_u32(header_.data() + 12U);
    if (static_cast<std::size_t>(payload_size) > max_frame_size_ - kFrameHeaderSize) {
        fail(common::ErrorCode::frame_too_large);
        return common::Result<void>::failure(common::ErrorCode::frame_too_large,
                                             "remote frame exceeds the configured size limit");
    }

    current_type_ = *type;
    current_flags_ = flags;
    current_request_id_ = read_u64(header_.data() + 16U);
    expected_payload_size_ = payload_size;
    return common::Result<void>::success();
}

void FrameDecoder::fail(const common::ErrorCode code) noexcept {
    common::secure_erase_memory(payload_.data(), payload_.size());
    header_.fill(0U);
    header_size_ = 0U;
    reset_current_frame();
    failed_ = true;
    failure_code_ = code;
}

void FrameDecoder::reset_current_frame() noexcept {
    common::secure_erase_memory(payload_.data(), payload_.size());
    payload_.clear();
    expected_payload_size_ = 0U;
    current_type_ = MessageType::hello;
    current_flags_ = 0U;
    current_request_id_ = 0U;
    reading_payload_ = false;
}

} // namespace minitun::protocol
