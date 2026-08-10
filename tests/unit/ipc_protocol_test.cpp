#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/ipc/protocol.hpp>

namespace minitun::ipc {
namespace {

constexpr std::string_view kRequestId{"req_0123456789abcdef0123456789abcdef"};

[[nodiscard]] common::Id make_id(const std::string_view value, const common::IdKind kind) {
    return common::Id::parse(value, kind).value();
}

[[nodiscard]] common::Id make_request_id() { return make_id(kRequestId, common::IdKind::request); }

[[nodiscard]] Request make_request(Json params = Json::object()) {
    return Request{kProtocolVersion, make_request_id(), "daemon.status", std::move(params)};
}

[[nodiscard]] std::string valid_request_payload() {
    return std::string{"{\"version\":1,\"request_id\":\""} + std::string{kRequestId} +
           "\",\"method\":\"daemon.status\",\"params\":{}}";
}

TEST(IpcProtocolTest, AcceptsTheBoundedDottedMethodGrammar) {
    constexpr std::array<std::string_view, 5> valid{"status", "daemon.status", "tun.add",
                                                    "server.login_v2", "a.b-c"};
    for (const auto method : valid) {
        EXPECT_TRUE(validate_method_name(method)) << method;
    }

    const std::string maximum(kMaxMethodBytes, 'a');
    EXPECT_TRUE(validate_method_name(maximum));
}

TEST(IpcProtocolTest, RejectsMalformedOrOversizedMethodNames) {
    const std::array<std::string, 10> invalid{
        "",
        ".status",
        "daemon.",
        "daemon..status",
        "Daemon.status",
        "1daemon.status",
        "daemon status",
        "daemon/status",
        "daemon._status",
        std::string(kMaxMethodBytes + 1U, 'a'),
    };

    for (const auto& method : invalid) {
        const auto result = validate_method_name(method);
        ASSERT_FALSE(result) << method;
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument) << method;
    }
}

TEST(IpcProtocolTest, RoundTripsARequestWithStableCompactEncoding) {
    Request request = make_request(Json{{"verbose", false}});

    const auto serialized = serialize_request(request);
    ASSERT_TRUE(serialized) << serialized.error();
    EXPECT_EQ(*serialized, std::string{"{\"version\":1,\"request_id\":\""} +
                               std::string{kRequestId} +
                               "\",\"method\":\"daemon.status\",\"params\":{\"verbose\":false}}");

    const auto parsed = parse_request(*serialized);
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(*parsed, request);
}

TEST(IpcProtocolTest, ParsesRequestFieldsIndependentlyOfInputOrder) {
    const std::string payload =
        std::string{"{\"params\":{},\"method\":\"daemon.status\",\"request_id\":\""} +
        std::string{kRequestId} + "\",\"version\":1}";

    const auto parsed = parse_request(payload);

    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->version, kProtocolVersion);
    EXPECT_EQ(parsed->request_id.str(), kRequestId);
    EXPECT_EQ(parsed->method, "daemon.status");
    EXPECT_EQ(parsed->params, Json::object());
}

TEST(IpcProtocolTest, StrictlyRejectsMalformedRequestEnvelopes) {
    const std::string id{kRequestId};
    const std::array invalid{
        std::string{"[]"},
        std::string{"{}"},
        std::string{"{\"version\":1,\"request_id\":\""} + id + "\",\"method\":\"daemon.status\"}",
        std::string{"{\"version\":1,\"request_id\":\""} + id +
            "\",\"method\":\"daemon.status\",\"params\":{},\"extra\":1}",
        std::string{"{\"version\":true,\"request_id\":\""} + id +
            "\",\"method\":\"daemon.status\",\"params\":{}}",
        std::string{"{\"version\":1.0,\"request_id\":\""} + id +
            "\",\"method\":\"daemon.status\",\"params\":{}}",
        std::string{"{\"version\":1,\"request_id\":7,\"method\":\"daemon.status\","
                    "\"params\":{}}"},
        std::string{"{\"version\":1,\"request_id\":\"srv_0123456789abcdef0123456789abcdef\","
                    "\"method\":\"daemon.status\",\"params\":{}}"},
        std::string{"{\"version\":1,\"request_id\":\""} + id + "\",\"method\":7,\"params\":{}}",
        std::string{"{\"version\":1,\"request_id\":\""} + id +
            "\",\"method\":\"daemon..status\",\"params\":{}}",
        std::string{"{\"version\":1,\"request_id\":\""} + id +
            "\",\"method\":\"daemon.status\",\"params\":[]}",
    };

    for (const auto& payload : invalid) {
        const auto parsed = parse_request(payload);
        ASSERT_FALSE(parsed) << payload;
        EXPECT_EQ(parsed.error().code(), common::ErrorCode::invalid_argument) << payload;
    }
}

TEST(IpcProtocolTest, DistinguishesUnsupportedVersionsFromMalformedVersions) {
    std::string payload = valid_request_payload();
    payload.replace(payload.find("\"version\":1"), std::string{"\"version\":1"}.size(),
                    "\"version\":2");

    const auto parsed = parse_request(payload);

    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code(), common::ErrorCode::unsupported_version);

    Request request = make_request();
    request.version = 2U;
    const auto serialized = serialize_request(request);
    ASSERT_FALSE(serialized);
    EXPECT_EQ(serialized.error().code(), common::ErrorCode::unsupported_version);
}

TEST(IpcProtocolTest, RejectsEmptyMalformedAndInvalidUtf8DocumentsWithoutEchoingThem) {
    const auto empty = parse_request("");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), common::ErrorCode::invalid_argument);

    const auto malformed = parse_request("{not-json}");
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(malformed.error().message(), "IPC payload is not valid UTF-8 JSON");

    std::string invalid_utf8 = "{\"version\":1,\"request_id\":\"";
    invalid_utf8 += kRequestId;
    invalid_utf8 += "\",\"method\":\"daemon.";
    invalid_utf8.push_back(static_cast<char>(0xff));
    invalid_utf8 += "\",\"params\":{}}";
    const auto invalid = parse_request(invalid_utf8);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(invalid.error().message(), "IPC payload is not valid UTF-8 JSON");
    EXPECT_EQ(invalid.error().message().find(static_cast<char>(0xff)), std::string::npos);
}

TEST(IpcProtocolTest, RejectsUtf8BomAndDuplicateObjectKeys) {
    std::string with_bom{"\xef\xbb\xbf", 3U};
    with_bom += valid_request_payload();
    const auto bom = parse_request(with_bom);
    ASSERT_FALSE(bom);
    EXPECT_EQ(bom.error().code(), common::ErrorCode::invalid_argument);

    const std::string duplicate_top_level =
        std::string{"{\"version\":1,\"version\":1,\"request_id\":\""} + std::string{kRequestId} +
        "\",\"method\":\"daemon.status\",\"params\":{}}";
    const auto top_level = parse_request(duplicate_top_level);
    ASSERT_FALSE(top_level);
    EXPECT_EQ(top_level.error().code(), common::ErrorCode::invalid_argument);

    const std::string duplicate_nested =
        std::string{"{\"version\":1,\"request_id\":\""} + std::string{kRequestId} +
        "\",\"method\":\"daemon.status\",\"params\":{\"key\":1,\"key\":2}}";
    const auto nested = parse_request(duplicate_nested);
    ASSERT_FALSE(nested);
    EXPECT_EQ(nested.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcProtocolTest, RejectsJsonCommentsInRequestsAndResponses) {
    const std::string commented_request = "/* not JSON */" + valid_request_payload();
    const auto request = parse_request(commented_request);
    ASSERT_FALSE(request);
    EXPECT_EQ(request.error().code(), common::ErrorCode::invalid_argument);

    const std::string commented_response =
        std::string{"{\"version\":1,// not JSON\n\"request_id\":\""} + std::string{kRequestId} +
        "\",\"ok\":true,\"result\":{}}";
    const auto response = parse_response(commented_response);
    ASSERT_FALSE(response);
    EXPECT_EQ(response.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcProtocolTest, AppliesConfiguredAndAbsoluteMessageSizeLimits) {
    const std::string payload = valid_request_payload();
    const auto parse_too_large = parse_request(payload, payload.size() - 1U);
    ASSERT_FALSE(parse_too_large);
    EXPECT_EQ(parse_too_large.error().code(), common::ErrorCode::frame_too_large);

    const auto serialize_too_large = serialize_request(make_request(), 1U);
    ASSERT_FALSE(serialize_too_large);
    EXPECT_EQ(serialize_too_large.error().code(), common::ErrorCode::frame_too_large);

    const std::string absolute_limit_plus_one(kDefaultMaxFrameSize + 1U, 'x');
    const auto absolute = parse_request(absolute_limit_plus_one, kDefaultMaxFrameSize + 4096U);
    ASSERT_FALSE(absolute);
    EXPECT_EQ(absolute.error().code(), common::ErrorCode::frame_too_large);
}

TEST(IpcProtocolTest, RejectsJsonBeyondTheMaximumNestingDepth) {
    Json nested = nullptr;
    for (std::size_t depth = 0; depth < kMaxJsonDepth + 2U; ++depth) {
        nested = Json::array({std::move(nested)});
    }
    Json params = Json::object();
    params["nested"] = std::move(nested);

    const auto serialized = serialize_request(make_request(std::move(params)));

    ASSERT_FALSE(serialized);
    EXPECT_EQ(serialized.error().code(), common::ErrorCode::resource_exhausted);

    std::string deep_payload = std::string{"{\"version\":1,\"request_id\":\""} +
                               std::string{kRequestId} +
                               "\",\"method\":\"daemon.status\",\"params\":{\"nested\":";
    deep_payload.append(kMaxJsonDepth + 2U, '[');
    deep_payload += '0';
    deep_payload.append(kMaxJsonDepth + 2U, ']');
    deep_payload += "}}";
    const auto parsed = parse_request(deep_payload);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code(), common::ErrorCode::resource_exhausted);
}

TEST(IpcProtocolTest, RejectsOversizedJsonStringsAndNodeCounts) {
    const std::string long_string_payload =
        std::string{"{\"version\":1,\"request_id\":\""} + std::string{kRequestId} +
        "\",\"method\":\"daemon.status\",\"params\":{\"value\":\"" +
        std::string(kMaxJsonStringBytes + 1U, 'x') + "\"}}";
    const auto string_result = parse_request(long_string_payload);
    ASSERT_FALSE(string_result);
    EXPECT_EQ(string_result.error().code(), common::ErrorCode::resource_exhausted);

    Json values = Json::array();
    values.get_ref<Json::array_t&>().resize(kMaxJsonNodes, nullptr);
    Json params = Json::object();
    params["values"] = std::move(values);
    const auto node_result = serialize_request(make_request(std::move(params)));
    ASSERT_FALSE(node_result);
    EXPECT_EQ(node_result.error().code(), common::ErrorCode::resource_exhausted);
}

TEST(IpcProtocolTest, RejectsInvalidInMemoryRequestValues) {
    Json invalid_utf8 = Json::object();
    invalid_utf8["value"] = std::string(1U, static_cast<char>(0xff));
    const auto utf8 = serialize_request(make_request(std::move(invalid_utf8)));
    ASSERT_FALSE(utf8);
    EXPECT_EQ(utf8.error().code(), common::ErrorCode::invalid_argument);

    Json non_finite = Json::object();
    non_finite["value"] = std::numeric_limits<double>::infinity();
    const auto infinity = serialize_request(make_request(std::move(non_finite)));
    ASSERT_FALSE(infinity);
    EXPECT_EQ(infinity.error().code(), common::ErrorCode::invalid_argument);

    Request array_params = make_request();
    array_params.params = Json::array();
    const auto array = serialize_request(array_params);
    ASSERT_FALSE(array);
    EXPECT_EQ(array.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcProtocolTest, RoundTripsSuccessAndFailureResponses) {
    const Response success = Response::success(make_request_id(), Json{{"state", "running"}});
    const auto success_payload = serialize_response(success);
    ASSERT_TRUE(success_payload) << success_payload.error();
    const auto parsed_success = parse_response(*success_payload);
    ASSERT_TRUE(parsed_success) << parsed_success.error();
    EXPECT_EQ(*parsed_success, success);
    EXPECT_TRUE(parsed_success->ok());
    ASSERT_NE(parsed_success->result(), nullptr);
    EXPECT_EQ(*parsed_success->result(), Json({{"state", "running"}}));
    EXPECT_EQ(parsed_success->error(), nullptr);

    const Response failure = Response::failure(
        make_request_id(), common::Error{common::ErrorCode::invalid_argument, "bad request"});
    const auto failure_payload = serialize_response(failure);
    ASSERT_TRUE(failure_payload) << failure_payload.error();
    const auto parsed_failure = parse_response(*failure_payload);
    ASSERT_TRUE(parsed_failure) << parsed_failure.error();
    EXPECT_EQ(*parsed_failure, failure);
    EXPECT_FALSE(parsed_failure->ok());
    EXPECT_EQ(parsed_failure->result(), nullptr);
    ASSERT_NE(parsed_failure->error(), nullptr);
    EXPECT_EQ(parsed_failure->error()->code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(parsed_failure->error()->message(), "bad request");
}

TEST(IpcProtocolTest, StrictlyRejectsMalformedResponseEnvelopes) {
    const std::string prefix =
        std::string{"{\"version\":1,\"request_id\":\""} + std::string{kRequestId} + "\",";
    const std::array invalid{
        std::string{"[]"},
        prefix + "\"ok\":true}",
        prefix + "\"ok\":true,\"result\":[]}",
        prefix + "\"ok\":true,\"result\":{},\"error\":{}}",
        prefix + "\"ok\":false}",
        prefix + "\"ok\":false,\"result\":{}}",
        prefix + "\"ok\":false,\"error\":null}",
        prefix + "\"ok\":false,\"error\":{\"code\":\"ok\",\"message\":\"\"}}",
        prefix + "\"ok\":false,\"error\":{\"code\":\"unknown\",\"message\":\"x\"}}",
        prefix + "\"ok\":false,\"error\":{\"code\":7,\"message\":\"x\"}}",
        prefix + "\"ok\":false,\"error\":{\"code\":\"ipc_error\"}}",
        prefix + "\"ok\":false,\"error\":{\"code\":\"ipc_error\",\"message\":\"x\",\"extra\":1}}",
        prefix + "\"ok\":1,\"result\":{}}",
        prefix + "\"ok\":true,\"result\":{},\"extra\":1}",
    };

    for (const auto& payload : invalid) {
        const auto parsed = parse_response(payload);
        ASSERT_FALSE(parsed) << payload;
        EXPECT_EQ(parsed.error().code(), common::ErrorCode::invalid_argument) << payload;
    }
}

TEST(IpcProtocolTest, EnforcesResponseVersionIdentifierAndSize) {
    const std::string unsupported = std::string{"{\"version\":2,\"request_id\":\""} +
                                    std::string{kRequestId} + "\",\"ok\":true,\"result\":{}}";
    const auto version = parse_response(unsupported);
    ASSERT_FALSE(version);
    EXPECT_EQ(version.error().code(), common::ErrorCode::unsupported_version);

    const std::string wrong_id =
        "{\"version\":1,\"request_id\":\"srv_0123456789abcdef0123456789abcdef\","
        "\"ok\":true,\"result\":{}}";
    const auto identifier = parse_response(wrong_id);
    ASSERT_FALSE(identifier);
    EXPECT_EQ(identifier.error().code(), common::ErrorCode::invalid_argument);

    const Response response = Response::success(make_request_id(), Json::object());
    const auto too_large = serialize_response(response, 1U);
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().code(), common::ErrorCode::frame_too_large);

    const Response invalid_result = Response::success(make_request_id(), Json::array());
    const auto invalid = serialize_response(invalid_result);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcProtocolTest, RejectsOversizedErrorMessagesOnTheWire) {
    const std::string payload =
        std::string{"{\"version\":1,\"request_id\":\""} + std::string{kRequestId} +
        "\",\"ok\":false,\"error\":{\"code\":\"internal_error\",\"message\":\"" +
        std::string(kMaxErrorMessageBytes + 1U, 'x') + "\"}}";

    const auto parsed = parse_response(payload);

    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcProtocolTest, SanitizesInvalidFailureObjectsBeforeSerialization) {
    const Response ok_error = Response::failure(
        make_request_id(), common::Error{common::ErrorCode::ok, "must not escape"});
    ASSERT_NE(ok_error.error(), nullptr);
    EXPECT_EQ(ok_error.error()->code(), common::ErrorCode::internal_error);
    EXPECT_EQ(ok_error.error()->message(), "request failed");

    const Response oversized = Response::failure(
        make_request_id(), common::Error{common::ErrorCode::internal_error,
                                         std::string(kMaxErrorMessageBytes + 1U, 's')});
    ASSERT_NE(oversized.error(), nullptr);
    EXPECT_EQ(oversized.error()->code(), common::ErrorCode::internal_error);
    EXPECT_EQ(oversized.error()->message(), "request failed");

    const Response invalid_utf8 = Response::failure(
        make_request_id(),
        common::Error{common::ErrorCode::internal_error, std::string(1U, static_cast<char>(0xff))});
    ASSERT_NE(invalid_utf8.error(), nullptr);
    EXPECT_EQ(invalid_utf8.error()->message(), "request failed");
    EXPECT_TRUE(serialize_response(invalid_utf8));
}

TEST(IpcProtocolTest, RejectsAResponseWhoseIdentifierIsNotARequestId) {
    const common::Id server_id =
        make_id("srv_0123456789abcdef0123456789abcdef", common::IdKind::server);
    const Response response = Response::success(server_id, Json::object());

    const auto serialized = serialize_response(response);

    ASSERT_FALSE(serialized);
    EXPECT_EQ(serialized.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcProtocolTest, ValidatesEveryCanonicalUtf8BoundaryInMemoryAndOnWire) {
    constexpr std::array<std::string_view, 12> valid_sequences{
        "\xc2\xa2",         "\xdf\xbf",         "\xe0\xa0\x80",     "\xed\x9f\xbf",
        "\xe1\x80\x80",     "\xec\xbf\xbf",     "\xee\x80\x80",     "\xef\xbf\xbf",
        "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf", "\xf1\x80\x80\x80", "\xf3\xbf\xbf\xbf",
    };
    for (const auto sequence : valid_sequences) {
        Request request = make_request(Json{{"value", std::string{sequence}}});
        const auto serialized = serialize_request(request);
        ASSERT_TRUE(serialized) << sequence;
        const auto parsed = parse_request(*serialized);
        ASSERT_TRUE(parsed) << parsed.error();
        EXPECT_EQ(parsed->params.at("value"), sequence);
    }

    constexpr std::array<std::string_view, 13> invalid_sequences{
        "\xc1\x80",
        "\xc2"
        "A",
        "\xe0\x9f\x80",
        "\xe0\xa0",
        "\xed\xa0\x80",
        "\xe1\x80"
        "A",
        "\xf0\x8f\x80\x80",
        "\xf4\x90\x80\x80",
        "\xf1\x80\x80"
        "A",
        "\xf0\x90"
        "A\x80",
        "\xf0\x90\x80"
        "A",
        "\xf0\x90\x80",
        "\xf5\x80\x80\x80",
    };
    for (const auto sequence : invalid_sequences) {
        const auto serialized =
            serialize_request(make_request(Json{{"value", std::string{sequence}}}));
        ASSERT_FALSE(serialized) << sequence;
        EXPECT_EQ(serialized.error().code(), common::ErrorCode::invalid_argument);

        std::string wire = std::string{"{\"version\":1,\"request_id\":\""} +
                           std::string{kRequestId} +
                           "\",\"method\":\"daemon.status\",\"params\":{\"value\":\"";
        wire.append(sequence).append("\"}}");
        const auto parsed = parse_request(wire);
        ASSERT_FALSE(parsed) << sequence;
        EXPECT_EQ(parsed.error().code(), common::ErrorCode::invalid_argument);
    }
}

TEST(IpcProtocolTest, RejectsEveryNonJsonInMemoryValueAndObjectKeyLimit) {
    Json binary_params = Json::object();
    binary_params["value"] = Json::binary({0x01U, 0x02U});
    const auto binary = serialize_request(make_request(std::move(binary_params)));
    ASSERT_FALSE(binary);
    EXPECT_EQ(binary.error().code(), common::ErrorCode::invalid_argument);

    Json discarded_params = Json::object();
    discarded_params["value"] = Json::parse("not-json", nullptr, false);
    ASSERT_TRUE(discarded_params.at("value").is_discarded());
    const auto discarded = serialize_request(make_request(std::move(discarded_params)));
    ASSERT_FALSE(discarded);
    EXPECT_EQ(discarded.error().code(), common::ErrorCode::invalid_argument);

    Json oversized_value = Json::object();
    oversized_value["value"] = std::string(kMaxJsonStringBytes + 1U, 'v');
    const auto value = serialize_request(make_request(std::move(oversized_value)));
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().code(), common::ErrorCode::resource_exhausted);

    Json oversized_key = Json::object();
    oversized_key[std::string(kMaxJsonStringBytes + 1U, 'k')] = true;
    const auto key = serialize_request(make_request(std::move(oversized_key)));
    ASSERT_FALSE(key);
    EXPECT_EQ(key.error().code(), common::ErrorCode::resource_exhausted);

    std::string malformed_key(1U, static_cast<char>(0xffU));
    Json invalid_key = Json::object();
    invalid_key[malformed_key] = true;
    const auto key_utf8 = serialize_request(make_request(std::move(invalid_key)));
    ASSERT_FALSE(key_utf8);
    EXPECT_EQ(key_utf8.error().code(), common::ErrorCode::invalid_argument);
}

TEST(IpcProtocolTest, RejectsWireKeyAndNodeLimitsAndSignedUnsupportedVersions) {
    const std::string oversized_key = std::string{"{\"version\":1,\"request_id\":\""} +
                                      std::string{kRequestId} +
                                      "\",\"method\":\"daemon.status\",\"params\":{\"" +
                                      std::string(kMaxJsonStringBytes + 1U, 'k') + "\":true}}";
    const auto key = parse_request(oversized_key);
    ASSERT_FALSE(key);
    EXPECT_EQ(key.error().code(), common::ErrorCode::resource_exhausted);

    std::string too_many_nodes = std::string{"{\"version\":1,\"request_id\":\""} +
                                 std::string{kRequestId} +
                                 "\",\"method\":\"daemon.status\",\"params\":{\"values\":[";
    for (std::size_t index = 0U; index < kMaxJsonNodes; ++index) {
        if (index != 0U) {
            too_many_nodes.push_back(',');
        }
        too_many_nodes.append("null");
    }
    too_many_nodes.append("]}}");
    const auto nodes = parse_request(too_many_nodes);
    ASSERT_FALSE(nodes);
    EXPECT_EQ(nodes.error().code(), common::ErrorCode::resource_exhausted);

    std::string signed_version = valid_request_payload();
    signed_version.replace(signed_version.find("\"version\":1"),
                           std::string_view{"\"version\":1"}.size(), "\"version\":-1");
    const auto version = parse_request(signed_version);
    ASSERT_FALSE(version);
    EXPECT_EQ(version.error().code(), common::ErrorCode::unsupported_version);
}

TEST(IpcProtocolTest, RejectsInvalidInMemoryRequestIdentityAndMethod) {
    Request wrong_id = make_request();
    wrong_id.request_id = make_id("srv_0123456789abcdef0123456789abcdef", common::IdKind::server);
    const auto identity = serialize_request(wrong_id);
    ASSERT_FALSE(identity);
    EXPECT_EQ(identity.error().code(), common::ErrorCode::invalid_argument);

    Request wrong_method = make_request();
    wrong_method.method = "daemon..status";
    const auto method = serialize_request(wrong_method);
    ASSERT_FALSE(method);
    EXPECT_EQ(method.error().code(), common::ErrorCode::invalid_argument);
}

} // namespace
} // namespace minitun::ipc
