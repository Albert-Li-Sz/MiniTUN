#include <minitun/ipc/protocol.hpp>

#include <algorithm>
#include <cmath>
#include <new>
#include <set>
#include <utility>
#include <vector>

namespace minitun::ipc {
namespace {

[[nodiscard]] std::size_t effective_limit(const std::size_t requested_limit) noexcept {
    return std::min(requested_limit, kDefaultMaxFrameSize);
}

[[nodiscard]] bool is_continuation(const unsigned char byte) noexcept {
    return byte >= 0x80U && byte <= 0xbfU;
}

[[nodiscard]] bool is_valid_utf8(const std::string_view input) noexcept {
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1U >= input.size() ||
                !is_continuation(static_cast<unsigned char>(input[index + 1U]))) {
                return false;
            }
            index += 2U;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2U >= input.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(input[index + 1U]);
            const auto third = static_cast<unsigned char>(input[index + 2U]);
            const bool second_valid = first == 0xe0U   ? second >= 0xa0U && second <= 0xbfU
                                      : first == 0xedU ? second >= 0x80U && second <= 0x9fU
                                                       : is_continuation(second);
            if (!second_valid || !is_continuation(third)) {
                return false;
            }
            index += 3U;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3U >= input.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(input[index + 1U]);
            const auto third = static_cast<unsigned char>(input[index + 2U]);
            const auto fourth = static_cast<unsigned char>(input[index + 3U]);
            const bool second_valid = first == 0xf0U   ? second >= 0x90U && second <= 0xbfU
                                      : first == 0xf4U ? second >= 0x80U && second <= 0x8fU
                                                       : is_continuation(second);
            if (!second_valid || !is_continuation(third) || !is_continuation(fourth)) {
                return false;
            }
            index += 4U;
            continue;
        }

        return false;
    }
    return true;
}

[[nodiscard]] bool is_request_field(const std::string_view field) noexcept {
    return field == "version" || field == "request_id" || field == "method" || field == "params";
}

[[nodiscard]] bool is_response_base_field(const std::string_view field) noexcept {
    return field == "version" || field == "request_id" || field == "ok";
}

[[nodiscard]] common::Result<void> validate_version(const Json& value) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC version must be an integer");
    }

    bool supported = false;
    if (value.is_number_unsigned()) {
        supported = value.get<std::uint64_t>() == kProtocolVersion;
    } else {
        supported = value.get<std::int64_t>() == kProtocolVersion;
    }
    if (!supported) {
        return common::Result<void>::failure(common::ErrorCode::unsupported_version,
                                             "unsupported IPC protocol version");
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> validate_request_id(const common::Id& id) {
    if (id.kind() != common::IdKind::request) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "request_id must be a canonical request ID");
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void>
validate_json_tree_impl(const Json& value, const std::size_t depth, std::size_t& nodes) {
    if ((value.is_array() || value.is_object()) && depth >= kMaxJsonDepth) {
        return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                             "JSON nesting exceeds the IPC limit");
    }
    if (value.is_discarded() || value.is_binary()) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC payload contains a non-JSON value");
    }
    ++nodes;
    if (nodes > kMaxJsonNodes) {
        return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                             "JSON node count exceeds the IPC limit");
    }
    if (value.is_string()) {
        const auto& string = value.get_ref<const std::string&>();
        if (string.size() > kMaxJsonStringBytes) {
            return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                                 "JSON string exceeds the IPC limit");
        }
        if (!is_valid_utf8(string)) {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "IPC payload contains invalid UTF-8");
        }
    }
    if (value.is_number_float() && !std::isfinite(value.get_ref<const Json::number_float_t&>())) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC payload contains a non-finite number");
    }
    if (value.is_array()) {
        for (const auto& element : value) {
            auto valid = validate_json_tree_impl(element, depth + 1U, nodes);
            if (!valid) {
                return valid;
            }
        }
    }
    if (value.is_object()) {
        for (auto item = value.cbegin(); item != value.cend(); ++item) {
            ++nodes;
            if (nodes > kMaxJsonNodes) {
                return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                                     "JSON node count exceeds the IPC limit");
            }
            if (item.key().size() > kMaxJsonStringBytes) {
                return common::Result<void>::failure(common::ErrorCode::resource_exhausted,
                                                     "JSON string exceeds the IPC limit");
            }
            if (!is_valid_utf8(item.key())) {
                return common::Result<void>::failure(
                    common::ErrorCode::invalid_argument,
                    "IPC payload contains an invalid UTF-8 object key");
            }
            auto valid = validate_json_tree_impl(item.value(), depth + 1U, nodes);
            if (!valid) {
                return valid;
            }
        }
    }
    return common::Result<void>::success();
}

[[nodiscard]] common::Result<void> validate_json_tree(const Json& value) {
    std::size_t nodes = 0;
    return validate_json_tree_impl(value, 0, nodes);
}

enum class ParseLimitFailure : std::uint8_t {
    none,
    duplicate_key,
    depth,
    nodes,
    string,
};

struct ParseContext final {
    bool object;
    std::set<std::string, std::less<>> keys;
};

struct ParseLimits final {
    ParseLimitFailure failure{ParseLimitFailure::none};
    std::size_t nodes{0};
    std::vector<ParseContext> contexts;

    [[nodiscard]] bool accept(const int, const Json::parse_event_t event, Json& parsed) {
        if (failure != ParseLimitFailure::none) {
            return false;
        }

        if (event == Json::parse_event_t::object_start ||
            event == Json::parse_event_t::array_start) {
            if (contexts.size() >= kMaxJsonDepth) {
                failure = ParseLimitFailure::depth;
                return false;
            }
            if (++nodes > kMaxJsonNodes) {
                failure = ParseLimitFailure::nodes;
                return false;
            }
            contexts.push_back(ParseContext{event == Json::parse_event_t::object_start, {}});
            return true;
        }

        if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) {
            if (!contexts.empty()) {
                contexts.pop_back();
            }
            return true;
        }

        if (event == Json::parse_event_t::key) {
            if (++nodes > kMaxJsonNodes) {
                failure = ParseLimitFailure::nodes;
                return false;
            }
            const auto& key = parsed.get_ref<const std::string&>();
            if (key.size() > kMaxJsonStringBytes) {
                failure = ParseLimitFailure::string;
                return false;
            }
            if (contexts.empty() || !contexts.back().object) {
                failure = ParseLimitFailure::nodes;
                return false;
            }
            if (!contexts.back().keys.emplace(key).second) {
                failure = ParseLimitFailure::duplicate_key;
                return false;
            }
            return true;
        }

        if (event == Json::parse_event_t::value) {
            if (++nodes > kMaxJsonNodes) {
                failure = ParseLimitFailure::nodes;
                return false;
            }
            if (parsed.is_string() &&
                parsed.get_ref<const std::string&>().size() > kMaxJsonStringBytes) {
                failure = ParseLimitFailure::string;
                return false;
            }
        }
        return true;
    }
};

[[nodiscard]] common::Error parse_limit_error(const ParseLimitFailure failure) {
    switch (failure) {
    case ParseLimitFailure::duplicate_key:
        return common::Error{common::ErrorCode::invalid_argument,
                             "IPC JSON contains a duplicate object key"};
    case ParseLimitFailure::depth:
        return common::Error{common::ErrorCode::resource_exhausted,
                             "JSON nesting exceeds the IPC limit"};
    case ParseLimitFailure::nodes:
        return common::Error{common::ErrorCode::resource_exhausted,
                             "JSON node count exceeds the IPC limit"};
    case ParseLimitFailure::string:
        return common::Error{common::ErrorCode::resource_exhausted,
                             "JSON string exceeds the IPC limit"};
    case ParseLimitFailure::none:
        break;
    }
    return common::Error{common::ErrorCode::invalid_argument,
                         "IPC payload is not valid UTF-8 JSON"};
}

[[nodiscard]] common::Result<Json> parse_document(const std::string_view payload,
                                                  const std::size_t max_message_size) {
    if (payload.size() > effective_limit(max_message_size)) {
        return common::Result<Json>::failure(common::ErrorCode::frame_too_large,
                                             "IPC message exceeds the configured size limit");
    }
    if (payload.empty()) {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "IPC JSON payload must not be empty");
    }
    if (payload.size() >= 3U && static_cast<unsigned char>(payload[0]) == 0xefU &&
        static_cast<unsigned char>(payload[1]) == 0xbbU &&
        static_cast<unsigned char>(payload[2]) == 0xbfU) {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "IPC JSON must not contain a UTF-8 BOM");
    }
    if (!is_valid_utf8(payload)) {
        return common::Result<Json>::failure(common::ErrorCode::invalid_argument,
                                             "IPC payload is not valid UTF-8 JSON");
    }

    ParseLimits limits;
    const auto callback = [&limits](const int depth, const Json::parse_event_t event,
                                    Json& parsed) { return limits.accept(depth, event, parsed); };

    Json document = Json::parse(payload.cbegin(), payload.cend(), callback, true, false);
    if (limits.failure != ParseLimitFailure::none) {
        return common::Result<Json>::failure(parse_limit_error(limits.failure));
    }
    auto valid = validate_json_tree(document);
    if (!valid) {
        return common::Result<Json>::failure(valid.error());
    }
    return document;
}

[[nodiscard]] common::Result<std::string> dump_document(const nlohmann::ordered_json& document,
                                                        const std::size_t max_message_size) {
    std::string payload =
        document.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::strict);
    if (payload.size() > effective_limit(max_message_size)) {
        return common::Result<std::string>::failure(
            common::ErrorCode::frame_too_large, "IPC message exceeds the configured size limit");
    }
    return payload;
}

template <typename T>
[[nodiscard]] common::Result<T> invalid_json_error(const nlohmann::json::exception&) {
    return common::Result<T>::failure(common::ErrorCode::invalid_argument,
                                      "IPC payload is not valid UTF-8 JSON");
}

template <typename T> [[nodiscard]] common::Result<T> allocation_error() {
    return common::Result<T>::failure(common::ErrorCode::resource_exhausted,
                                      "insufficient memory while processing IPC JSON");
}

template <typename T> [[nodiscard]] common::Result<T> internal_json_error() {
    return common::Result<T>::failure(common::ErrorCode::internal_error,
                                      "unexpected IPC JSON processing failure");
}

} // namespace

Response Response::success(common::Id request_id, Json result) {
    return Response{std::move(request_id), std::move(result)};
}

Response Response::failure(common::Id request_id, common::Error error) {
    if (error.code() == common::ErrorCode::ok) {
        error = common::Error{common::ErrorCode::internal_error, "request failed"};
    } else if (error.message().size() > kMaxErrorMessageBytes || !is_valid_utf8(error.message())) {
        error = common::Error{error.code(), "request failed"};
    }
    return Response{std::move(request_id), std::move(error)};
}

std::uint16_t Response::version() const noexcept { return kProtocolVersion; }

const common::Id& Response::request_id() const noexcept { return request_id_; }

bool Response::ok() const noexcept { return std::holds_alternative<Json>(payload_); }

const Json* Response::result() const noexcept { return std::get_if<Json>(&payload_); }

const common::Error* Response::error() const noexcept {
    return std::get_if<common::Error>(&payload_);
}

Response::Response(common::Id request_id, std::variant<Json, common::Error> payload)
    : request_id_(std::move(request_id)), payload_(std::move(payload)) {}

common::Result<void> validate_method_name(const std::string_view method) {
    if (method.empty()) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC method must not be empty");
    }
    if (method.size() > kMaxMethodBytes) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC method exceeds the length limit");
    }

    bool beginning_of_segment = true;
    for (const char character : method) {
        if (character == '.') {
            if (beginning_of_segment) {
                return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                     "IPC method contains an empty segment");
            }
            beginning_of_segment = true;
            continue;
        }

        const bool lowercase = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (beginning_of_segment) {
            if (!lowercase) {
                return common::Result<void>::failure(
                    common::ErrorCode::invalid_argument,
                    "IPC method segments must begin with a lowercase letter");
            }
            beginning_of_segment = false;
            continue;
        }
        if (!lowercase && !digit && character != '_' && character != '-') {
            return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                                 "IPC method contains an invalid character");
        }
    }
    if (beginning_of_segment) {
        return common::Result<void>::failure(common::ErrorCode::invalid_argument,
                                             "IPC method contains an empty segment");
    }
    return common::Result<void>::success();
}

common::Result<Request> parse_request(const std::string_view payload,
                                      const std::size_t max_message_size) {
    try {
        auto parsed = parse_document(payload, max_message_size);
        if (!parsed) {
            return common::Result<Request>::failure(parsed.error());
        }
        Json document = std::move(parsed).value();
        if (!document.is_object()) {
            return common::Result<Request>::failure(common::ErrorCode::invalid_argument,
                                                    "IPC request must be a JSON object");
        }

        for (auto item = document.cbegin(); item != document.cend(); ++item) {
            if (!is_request_field(item.key())) {
                return common::Result<Request>::failure(
                    common::ErrorCode::invalid_argument,
                    "IPC request contains an unknown top-level field");
            }
        }

        const auto version = document.find("version");
        const auto request_id = document.find("request_id");
        const auto method = document.find("method");
        const auto params = document.find("params");
        if (version == document.end() || request_id == document.end() || method == document.end() ||
            params == document.end()) {
            return common::Result<Request>::failure(
                common::ErrorCode::invalid_argument,
                "IPC request is missing a required top-level field");
        }

        auto valid_version = validate_version(*version);
        if (!valid_version) {
            return common::Result<Request>::failure(valid_version.error());
        }
        if (!request_id->is_string()) {
            return common::Result<Request>::failure(common::ErrorCode::invalid_argument,
                                                    "request_id must be a string");
        }
        auto id =
            common::Id::parse(request_id->get_ref<const std::string&>(), common::IdKind::request);
        if (!id) {
            return common::Result<Request>::failure(common::ErrorCode::invalid_argument,
                                                    "request_id must be a canonical request ID");
        }
        if (!method->is_string()) {
            return common::Result<Request>::failure(common::ErrorCode::invalid_argument,
                                                    "IPC method must be a string");
        }
        auto valid_method = validate_method_name(method->get_ref<const std::string&>());
        if (!valid_method) {
            return common::Result<Request>::failure(valid_method.error());
        }
        if (!params->is_object()) {
            return common::Result<Request>::failure(common::ErrorCode::invalid_argument,
                                                    "IPC params must be a JSON object");
        }

        return Request{kProtocolVersion, std::move(id).value(), method->get<std::string>(),
                       *params};
    } catch (const std::bad_alloc&) {
        return allocation_error<Request>();
    } catch (const nlohmann::json::exception& error) {
        return invalid_json_error<Request>(error);
    } catch (...) {
        return internal_json_error<Request>();
    }
}

common::Result<std::string> serialize_request(const Request& request,
                                              const std::size_t max_message_size) {
    try {
        if (request.version != kProtocolVersion) {
            return common::Result<std::string>::failure(common::ErrorCode::unsupported_version,
                                                        "unsupported IPC protocol version");
        }
        auto valid_id = validate_request_id(request.request_id);
        if (!valid_id) {
            return common::Result<std::string>::failure(valid_id.error());
        }
        auto valid_method = validate_method_name(request.method);
        if (!valid_method) {
            return common::Result<std::string>::failure(valid_method.error());
        }
        if (!request.params.is_object()) {
            return common::Result<std::string>::failure(common::ErrorCode::invalid_argument,
                                                        "IPC params must be a JSON object");
        }
        auto valid_params = validate_json_tree(request.params);
        if (!valid_params) {
            return common::Result<std::string>::failure(valid_params.error());
        }

        nlohmann::ordered_json document = nlohmann::ordered_json::object();
        document["version"] = request.version;
        document["request_id"] = request.request_id.str();
        document["method"] = request.method;
        document["params"] = request.params;
        const Json validated_document = document;
        auto valid_document = validate_json_tree(validated_document);
        if (!valid_document) {
            return common::Result<std::string>::failure(valid_document.error());
        }
        return dump_document(document, max_message_size);
    } catch (const std::bad_alloc&) {
        return allocation_error<std::string>();
    } catch (const nlohmann::json::exception& error) {
        return invalid_json_error<std::string>(error);
    } catch (...) {
        return internal_json_error<std::string>();
    }
}

common::Result<Response> parse_response(const std::string_view payload,
                                        const std::size_t max_message_size) {
    try {
        auto parsed = parse_document(payload, max_message_size);
        if (!parsed) {
            return common::Result<Response>::failure(parsed.error());
        }
        Json document = std::move(parsed).value();
        if (!document.is_object()) {
            return common::Result<Response>::failure(common::ErrorCode::invalid_argument,
                                                     "IPC response must be a JSON object");
        }

        const auto version = document.find("version");
        const auto request_id = document.find("request_id");
        const auto ok = document.find("ok");
        if (version == document.end() || request_id == document.end() || ok == document.end()) {
            return common::Result<Response>::failure(
                common::ErrorCode::invalid_argument,
                "IPC response is missing a required top-level field");
        }
        auto valid_version = validate_version(*version);
        if (!valid_version) {
            return common::Result<Response>::failure(valid_version.error());
        }
        if (!request_id->is_string()) {
            return common::Result<Response>::failure(common::ErrorCode::invalid_argument,
                                                     "request_id must be a string");
        }
        auto id =
            common::Id::parse(request_id->get_ref<const std::string&>(), common::IdKind::request);
        if (!id) {
            return common::Result<Response>::failure(common::ErrorCode::invalid_argument,
                                                     "request_id must be a canonical request ID");
        }
        if (!ok->is_boolean()) {
            return common::Result<Response>::failure(common::ErrorCode::invalid_argument,
                                                     "IPC response ok must be a boolean");
        }

        const bool succeeded = ok->get<bool>();
        for (auto item = document.cbegin(); item != document.cend(); ++item) {
            const bool allowed = is_response_base_field(item.key()) ||
                                 (succeeded ? item.key() == "result" : item.key() == "error");
            if (!allowed) {
                return common::Result<Response>::failure(
                    common::ErrorCode::invalid_argument,
                    "IPC response contains an unknown top-level field");
            }
        }

        if (succeeded) {
            const auto result = document.find("result");
            if (result == document.end()) {
                return common::Result<Response>::failure(
                    common::ErrorCode::invalid_argument,
                    "successful IPC response is missing result");
            }
            if (!result->is_object()) {
                return common::Result<Response>::failure(
                    common::ErrorCode::invalid_argument,
                    "successful IPC response result must be an object");
            }
            return Response::success(std::move(id).value(), *result);
        }

        const auto error_value = document.find("error");
        if (error_value == document.end() || !error_value->is_object()) {
            return common::Result<Response>::failure(
                common::ErrorCode::invalid_argument,
                "failed IPC response must contain an error object");
        }
        for (auto item = error_value->cbegin(); item != error_value->cend(); ++item) {
            if (item.key() != "code" && item.key() != "message") {
                return common::Result<Response>::failure(common::ErrorCode::invalid_argument,
                                                         "IPC error contains an unknown field");
            }
        }
        const auto code = error_value->find("code");
        const auto message = error_value->find("message");
        if (code == error_value->end() || message == error_value->end() || !code->is_string() ||
            !message->is_string()) {
            return common::Result<Response>::failure(common::ErrorCode::invalid_argument,
                                                     "IPC error code and message must be strings");
        }
        const auto parsed_code =
            common::error_code_from_string(code->get_ref<const std::string&>());
        if (!parsed_code.has_value() || *parsed_code == common::ErrorCode::ok) {
            return common::Result<Response>::failure(common::ErrorCode::invalid_argument,
                                                     "IPC error code is invalid");
        }
        const auto& error_message = message->get_ref<const std::string&>();
        if (error_message.size() > kMaxErrorMessageBytes) {
            return common::Result<Response>::failure(common::ErrorCode::invalid_argument,
                                                     "IPC error message exceeds the length limit");
        }
        return Response::failure(std::move(id).value(), common::Error{*parsed_code, error_message});
    } catch (const std::bad_alloc&) {
        return allocation_error<Response>();
    } catch (const nlohmann::json::exception& error) {
        return invalid_json_error<Response>(error);
    } catch (...) {
        return internal_json_error<Response>();
    }
}

common::Result<std::string> serialize_response(const Response& response,
                                               const std::size_t max_message_size) {
    try {
        auto valid_id = validate_request_id(response.request_id());
        if (!valid_id) {
            return common::Result<std::string>::failure(valid_id.error());
        }

        nlohmann::ordered_json document = nlohmann::ordered_json::object();
        document["version"] = response.version();
        document["request_id"] = response.request_id().str();
        document["ok"] = response.ok();
        if (response.ok()) {
            const Json* const result = response.result();
            if (result == nullptr) {
                return common::Result<std::string>::failure(
                    common::ErrorCode::internal_error,
                    "IPC response has an invalid success payload");
            }
            if (!result->is_object()) {
                return common::Result<std::string>::failure(
                    common::ErrorCode::invalid_argument,
                    "successful IPC response result must be an object");
            }
            auto valid_result = validate_json_tree(*result);
            if (!valid_result) {
                return common::Result<std::string>::failure(valid_result.error());
            }
            document["result"] = *result;
        } else {
            const common::Error* const error = response.error();
            if (error == nullptr || error->code() == common::ErrorCode::ok) {
                return common::Result<std::string>::failure(
                    common::ErrorCode::internal_error, "IPC response has an invalid error payload");
            }
            nlohmann::ordered_json error_document = nlohmann::ordered_json::object();
            error_document["code"] = common::to_string(error->code());
            error_document["message"] = error->message();
            document["error"] = std::move(error_document);
        }
        const Json validated_document = document;
        auto valid_document = validate_json_tree(validated_document);
        if (!valid_document) {
            return common::Result<std::string>::failure(valid_document.error());
        }
        return dump_document(document, max_message_size);
    } catch (const std::bad_alloc&) {
        return allocation_error<std::string>();
    } catch (const nlohmann::json::exception& error) {
        return invalid_json_error<std::string>(error);
    } catch (...) {
        return internal_json_error<std::string>();
    }
}

} // namespace minitun::ipc
