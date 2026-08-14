#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <minitun/common/result.hpp>
#include <minitun/protocol/frame.hpp>

namespace minitun::protocol {

class PayloadWriter final {
  public:
    explicit PayloadWriter(std::size_t max_payload_size = kMaxPayloadSize) noexcept;

    [[nodiscard]] common::Result<void> write_u8(std::uint8_t value);
    [[nodiscard]] common::Result<void> write_u16(std::uint16_t value);
    [[nodiscard]] common::Result<void> write_u32(std::uint32_t value);
    [[nodiscard]] common::Result<void> write_u64(std::uint64_t value);
    [[nodiscard]] common::Result<void> write_bytes(std::span<const std::uint8_t> value);
    [[nodiscard]] common::Result<void> write_string(std::string_view value);

    [[nodiscard]] common::Result<std::vector<std::uint8_t>> finish() &&;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    [[nodiscard]] common::Result<void> append(std::span<const std::uint8_t> value);
    [[nodiscard]] common::Result<void> fail(common::ErrorCode code, std::string message);

    std::size_t max_payload_size_;
    std::vector<std::uint8_t> bytes_;
    std::optional<common::Error> failure_;
};

class PayloadReader final {
  public:
    explicit PayloadReader(std::span<const std::uint8_t> bytes) noexcept;

    [[nodiscard]] common::Result<std::uint8_t> read_u8();
    [[nodiscard]] common::Result<std::uint16_t> read_u16();
    [[nodiscard]] common::Result<std::uint32_t> read_u32();
    [[nodiscard]] common::Result<std::uint64_t> read_u64();
    [[nodiscard]] common::Result<std::vector<std::uint8_t>> read_bytes(std::size_t max_length);
    [[nodiscard]] common::Result<std::string> read_string(std::size_t max_length);
    [[nodiscard]] common::Result<void> require_end() const;

    [[nodiscard]] std::size_t remaining() const noexcept;

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{0U};
};

[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept;

} // namespace minitun::protocol
