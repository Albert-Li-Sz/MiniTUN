#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/ipc/frame.hpp>

namespace minitun::ipc {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxMethodBytes = 128;
inline constexpr std::size_t kMaxErrorMessageBytes = 4096;
inline constexpr std::size_t kMaxJsonDepth = 64;
inline constexpr std::size_t kMaxJsonNodes = 65536;
inline constexpr std::size_t kMaxJsonStringBytes = 64U * 1024U;

using Json = nlohmann::json;

/// A validated version-1 IPC request.
struct Request final {
    std::uint16_t version;
    common::Id request_id;
    std::string method;
    Json params;

    friend bool operator==(const Request&, const Request&) = default;
};

/// A validated version-1 IPC response.
///
/// Construction through success() or failure() preserves the invariant that
/// exactly one of result or error is present on the wire.
class Response final {
  public:
    [[nodiscard]] static Response success(common::Id request_id, Json result);
    [[nodiscard]] static Response failure(common::Id request_id, common::Error error);

    [[nodiscard]] std::uint16_t version() const noexcept;
    [[nodiscard]] const common::Id& request_id() const noexcept;
    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] const Json* result() const noexcept;
    [[nodiscard]] const common::Error* error() const noexcept;

    friend bool operator==(const Response&, const Response&) = default;

  private:
    Response(common::Id request_id, std::variant<Json, common::Error> payload);

    common::Id request_id_;
    std::variant<Json, common::Error> payload_;
};

/// Validates MiniTun's bounded, dotted IPC method-name grammar.
[[nodiscard]] common::Result<void> validate_method_name(std::string_view method);

/// Parses and strictly validates an IPC request JSON document.
///
/// Unknown top-level fields are rejected. All four request fields are required,
/// params must be an object, and request_id must be a canonical req_ ID.
[[nodiscard]] common::Result<Request>
parse_request(std::string_view payload, std::size_t max_message_size = kDefaultMaxFrameSize);

/// Serializes a request using a stable field order and compact JSON encoding.
[[nodiscard]] common::Result<std::string>
serialize_request(const Request& request, std::size_t max_message_size = kDefaultMaxFrameSize);

/// Parses and strictly validates either the success or failure response shape.
[[nodiscard]] common::Result<Response>
parse_response(std::string_view payload, std::size_t max_message_size = kDefaultMaxFrameSize);

/// Serializes a response using a stable field order and compact JSON encoding.
[[nodiscard]] common::Result<std::string>
serialize_response(const Response& response, std::size_t max_message_size = kDefaultMaxFrameSize);

} // namespace minitun::ipc
