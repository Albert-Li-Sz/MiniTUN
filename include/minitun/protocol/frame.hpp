#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <minitun/common/result.hpp>

namespace minitun::protocol {

inline constexpr std::uint32_t kFrameMagic = 0x4D54554EU;
inline constexpr std::uint16_t kProtocolVersion = 1U;
inline constexpr std::size_t kFrameHeaderSize = 24U;
inline constexpr std::size_t kMaxFrameSize = 64U * 1024U;
inline constexpr std::size_t kMaxPayloadSize = kMaxFrameSize - kFrameHeaderSize;

enum class MessageType : std::uint16_t {
    hello = 1U,
    hello_ack = 2U,
    auth = 3U,
    auth_ok = 4U,
    auth_error = 5U,
    register_tunnel = 6U,
    register_tunnel_ok = 7U,
    register_tunnel_error = 8U,
    unregister_tunnel = 9U,
    unregister_tunnel_ok = 10U,
    request_workers = 11U,
    ping = 12U,
    pong = 13U,
    goaway = 14U,
    error = 15U,

    worker_hello = 0x0100U,
    worker_accepted = 0x0101U,
    start_relay = 0x0102U,
    local_connect_ok = 0x0103U,
    local_connect_error = 0x0104U,
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;
[[nodiscard]] std::optional<MessageType> message_type_from_wire(std::uint16_t value) noexcept;
[[nodiscard]] bool is_control_message(MessageType type) noexcept;
[[nodiscard]] bool is_worker_message(MessageType type) noexcept;

struct Frame final {
    MessageType type{MessageType::hello};
    std::uint32_t flags{0U};
    std::uint64_t request_id{0U};
    std::vector<std::uint8_t> payload;

    friend bool operator==(const Frame&, const Frame&) = default;
};

[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_frame(const Frame& frame, std::size_t max_frame_size = kMaxFrameSize);

class FrameDecoder final {
  public:
    explicit FrameDecoder(std::size_t max_frame_size = kMaxFrameSize) noexcept;
    ~FrameDecoder() noexcept;

    [[nodiscard]] common::Result<std::vector<Frame>>
    feed(std::span<const std::uint8_t> bytes);

    [[nodiscard]] common::Result<void> finish() const;
    void reset() noexcept;

    [[nodiscard]] std::size_t buffered_size() const noexcept;
    [[nodiscard]] std::size_t max_frame_size() const noexcept;

  private:
    [[nodiscard]] common::Result<void> parse_header();
    void fail(common::ErrorCode code) noexcept;
    void reset_current_frame() noexcept;

    std::size_t max_frame_size_;
    std::array<std::uint8_t, kFrameHeaderSize> header_{};
    std::size_t header_size_{0U};
    std::uint32_t expected_payload_size_{0U};
    MessageType current_type_{MessageType::hello};
    std::uint32_t current_flags_{0U};
    std::uint64_t current_request_id_{0U};
    std::vector<std::uint8_t> payload_;
    bool reading_payload_{false};
    bool failed_{false};
    common::ErrorCode failure_code_{common::ErrorCode::protocol_error};
};

} // namespace minitun::protocol
