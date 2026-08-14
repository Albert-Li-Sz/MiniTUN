#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <minitun/common/result.hpp>

namespace minitun::ipc {

/// IPC frames carry a four-byte, unsigned, network-byte-order payload length.
inline constexpr std::size_t kFrameHeaderSize = 4;

/// Default and absolute protocol limit for an IPC JSON payload (1 MiB).
inline constexpr std::size_t kDefaultMaxFrameSize = 1024U * 1024U;

/// Encodes one payload as a complete length-prefixed IPC frame.
///
/// The payload is treated as opaque bytes. UTF-8 and JSON validation belong to
/// protocol.hpp. A payload larger than max_frame_size is rejected before an
/// output allocation is attempted.
[[nodiscard]] common::Result<std::vector<std::uint8_t>>
encode_frame(std::string_view payload, std::size_t max_frame_size = kDefaultMaxFrameSize);

/// Incrementally decodes a stream of IPC length-prefixed frames.
///
/// feed() accepts arbitrary stream fragments and can return zero, one, or many
/// complete payloads. The decoder buffers at most one payload plus its header;
/// bytes belonging to later frames are consumed directly from the caller's
/// span. After an error, reset() must be called before the decoder can be used
/// for a new stream.
class FrameDecoder final {
  public:
    explicit FrameDecoder(std::size_t max_frame_size = kDefaultMaxFrameSize);
    ~FrameDecoder() noexcept;

    [[nodiscard]] common::Result<std::vector<std::string>>
    feed(std::span<const std::uint8_t> bytes);

    /// Verifies that the peer closed between frames rather than mid-frame.
    [[nodiscard]] common::Result<void> finish() const;

    /// Clears buffered data and any previous failure.
    void reset() noexcept;

    [[nodiscard]] std::size_t buffered_size() const noexcept;
    [[nodiscard]] std::size_t max_frame_size() const noexcept;

  private:
    void fail(common::ErrorCode code) noexcept;

    std::size_t max_frame_size_;
    std::uint32_t expected_payload_size_{0};
    std::size_t header_size_{0};
    std::uint8_t header_[kFrameHeaderSize]{};
    std::string payload_;
    bool reading_payload_{false};
    bool failed_{false};
    common::ErrorCode failure_code_{common::ErrorCode::protocol_error};
};

} // namespace minitun::ipc
