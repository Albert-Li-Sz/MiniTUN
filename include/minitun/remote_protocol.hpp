#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <minitun/common/result.hpp>
#include <minitun/protocol/frame.hpp>
#include <minitun/protocol/messages.hpp>

#if defined(_WIN32)
#if defined(MINITUN_REMOTE_PROTOCOL_BUILD)
#define MINITUN_REMOTE_PROTOCOL_API __declspec(dllexport)
#else
#define MINITUN_REMOTE_PROTOCOL_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define MINITUN_REMOTE_PROTOCOL_API __attribute__((visibility("default")))
#else
#define MINITUN_REMOTE_PROTOCOL_API
#endif

namespace minitun::remote {

struct EmptyMessage final {
    protocol::MessageType type{protocol::MessageType::goaway};
    friend bool operator==(const EmptyMessage&, const EmptyMessage&) = default;
};

using Message =
    std::variant<EmptyMessage, protocol::HelloMessage, protocol::HelloAckMessage,
                 protocol::AuthMessage, protocol::AuthOkMessage, protocol::AuthErrorMessage,
                 protocol::HeartbeatMessage, protocol::RegisterTunnelMessage,
                 protocol::RegisterTunnelOkMessage, protocol::RegisterTunnelErrorMessage,
                 protocol::UnregisterTunnelMessage, protocol::RequestWorkersMessage,
                 protocol::WorkerHelloMessage, protocol::WorkerAcceptedMessage,
                 protocol::StartRelayMessage, protocol::LocalConnectOkMessage,
                 protocol::LocalConnectErrorMessage>;

/// Incremental Remote Protocol v2 decoder suitable for TCP/TLS read loops.
class MINITUN_REMOTE_PROTOCOL_API Decoder final {
  public:
    explicit Decoder(std::size_t max_frame_size = protocol::kMaxFrameSize);
    ~Decoder() noexcept;
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    [[nodiscard]] common::Result<std::vector<protocol::Frame>>
    feed(std::span<const std::uint8_t> bytes);
    [[nodiscard]] common::Result<void> finish() const;
    void reset() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

class MINITUN_REMOTE_PROTOCOL_API Codec final {
  public:
    [[nodiscard]] static common::Result<std::vector<std::uint8_t>>
    encode_frame(const protocol::Frame& frame,
                 std::size_t max_frame_size = protocol::kMaxFrameSize);

    /// Encodes one typed message after verifying that it matches `type`.
    [[nodiscard]] static common::Result<protocol::Frame>
    make_frame(protocol::MessageType type, std::uint64_t request_id, const Message& message);

    [[nodiscard]] static common::Result<Message> decode_message(const protocol::Frame& frame);

    [[nodiscard]] static common::Result<protocol::AuthenticationData>
    control_authentication_data(std::string_view psk, std::string_view client_id,
                                std::string_view server_id, std::int64_t timestamp_seconds,
                                const protocol::AuthenticationNonce& nonce,
                                protocol::CapabilitySet selected_capabilities);

    [[nodiscard]] static common::Result<protocol::AuthenticationData>
    worker_authentication_data(std::string_view psk, std::string_view client_id,
                               std::string_view server_id, std::uint64_t session_generation,
                               std::string_view worker_id, std::int64_t timestamp_seconds,
                               const protocol::AuthenticationNonce& nonce);
};

} // namespace minitun::remote
