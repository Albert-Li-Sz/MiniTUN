#include <minitun/common/error.hpp>

#include <ostream>
#include <utility>

namespace minitun::common {

std::string_view to_string(const ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::ok:
        return "ok";
    case ErrorCode::invalid_argument:
        return "invalid_argument";
    case ErrorCode::not_found:
        return "not_found";
    case ErrorCode::already_exists:
        return "already_exists";
    case ErrorCode::permission_denied:
        return "permission_denied";
    case ErrorCode::not_authenticated:
        return "not_authenticated";
    case ErrorCode::authentication_failed:
        return "authentication_failed";
    case ErrorCode::connection_failed:
        return "connection_failed";
    case ErrorCode::connection_timeout:
        return "connection_timeout";
    case ErrorCode::remote_port_in_use:
        return "remote_port_in_use";
    case ErrorCode::local_connect_failed:
        return "local_connect_failed";
    case ErrorCode::protocol_error:
        return "protocol_error";
    case ErrorCode::frame_too_large:
        return "frame_too_large";
    case ErrorCode::unsupported_version:
        return "unsupported_version";
    case ErrorCode::resource_exhausted:
        return "resource_exhausted";
    case ErrorCode::database_error:
        return "database_error";
    case ErrorCode::tls_error:
        return "tls_error";
    case ErrorCode::ipc_error:
        return "ipc_error";
    case ErrorCode::internal_error:
        return "internal_error";
    }
    return "internal_error";
}

std::optional<ErrorCode> error_code_from_string(const std::string_view value) noexcept {
    if (value == "ok") {
        return ErrorCode::ok;
    }
    if (value == "invalid_argument") {
        return ErrorCode::invalid_argument;
    }
    if (value == "not_found") {
        return ErrorCode::not_found;
    }
    if (value == "already_exists") {
        return ErrorCode::already_exists;
    }
    if (value == "permission_denied") {
        return ErrorCode::permission_denied;
    }
    if (value == "not_authenticated") {
        return ErrorCode::not_authenticated;
    }
    if (value == "authentication_failed") {
        return ErrorCode::authentication_failed;
    }
    if (value == "connection_failed") {
        return ErrorCode::connection_failed;
    }
    if (value == "connection_timeout") {
        return ErrorCode::connection_timeout;
    }
    if (value == "remote_port_in_use") {
        return ErrorCode::remote_port_in_use;
    }
    if (value == "local_connect_failed") {
        return ErrorCode::local_connect_failed;
    }
    if (value == "protocol_error") {
        return ErrorCode::protocol_error;
    }
    if (value == "frame_too_large") {
        return ErrorCode::frame_too_large;
    }
    if (value == "unsupported_version") {
        return ErrorCode::unsupported_version;
    }
    if (value == "resource_exhausted") {
        return ErrorCode::resource_exhausted;
    }
    if (value == "database_error") {
        return ErrorCode::database_error;
    }
    if (value == "tls_error") {
        return ErrorCode::tls_error;
    }
    if (value == "ipc_error") {
        return ErrorCode::ipc_error;
    }
    if (value == "internal_error") {
        return ErrorCode::internal_error;
    }
    return std::nullopt;
}

Error::Error(const ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

ErrorCode Error::code() const noexcept { return code_; }

const std::string& Error::message() const noexcept { return message_; }

Error Error::with_context(const std::string_view context) const {
    if (context.empty()) {
        return *this;
    }

    std::string contextual_message;
    contextual_message.reserve(context.size() + (message_.empty() ? 0U : 2U) + message_.size());
    contextual_message.append(context);
    if (!message_.empty()) {
        contextual_message.append(": ");
        contextual_message.append(message_);
    }
    return Error{code_, std::move(contextual_message)};
}

std::ostream& operator<<(std::ostream& stream, const Error& error) {
    stream << to_string(error.code());
    if (!error.message().empty()) {
        stream << ": " << error.message();
    }
    return stream;
}

} // namespace minitun::common
