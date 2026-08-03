#pragma once

#include <chrono>
#include <cstddef>

#include <asio/error_code.hpp>
#include <asio/ip/tcp.hpp>

namespace minitun::server {

class AcceptRetryPolicy final {
  public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] std::chrono::milliseconds next_delay() noexcept;
    void reset() noexcept;
    [[nodiscard]] bool should_log(Clock::time_point now) noexcept;

    [[nodiscard]] static bool descriptor_exhausted(const asio::error_code& error) noexcept;
    [[nodiscard]] static bool resource_exhausted(const asio::error_code& error) noexcept;
    [[nodiscard]] static bool retryable(const asio::error_code& error) noexcept;

  private:
    std::size_t failure_count_{0U};
    Clock::time_point last_log_{};
    bool logged_{false};
};

// Keeps one descriptor in reserve so an EMFILE/ENFILE listener can accept and
// close one queued connection before entering a bounded retry delay.
class ReservedFileDescriptor final {
  public:
    ReservedFileDescriptor() noexcept;
    ~ReservedFileDescriptor() noexcept;

    ReservedFileDescriptor(const ReservedFileDescriptor&) = delete;
    ReservedFileDescriptor& operator=(const ReservedFileDescriptor&) = delete;

    void reopen() noexcept;
    void close() noexcept;
    void recover(asio::ip::tcp::acceptor& acceptor) noexcept;
    [[nodiscard]] bool available() const noexcept;

  private:
    int descriptor_{-1};
};

} // namespace minitun::server
