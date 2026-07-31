#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/protocol/messages.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::string generated_id(const common::IdKind kind) {
    auto id = common::Id::generate(kind);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

TEST(RemoteMessagesTest, RoundTripsHandshakeAndHeartbeatPayloads) {
    AuthenticationNonce nonce{};
    AuthenticationData digest{};
    for (std::size_t index = 0U; index < nonce.size(); ++index) {
        nonce[index] = static_cast<std::uint8_t>(index);
        digest[index] = static_cast<std::uint8_t>(0xffU - index);
    }

    const HelloMessage hello{generated_id(common::IdKind::client)};
    const HelloAckMessage ack{generated_id(common::IdKind::server), -123, nonce};
    const AuthMessage auth{hello.client_id, 456, nonce, digest};
    const AuthOkMessage ok{99U, 5'000U, 2U, 32U};
    const AuthErrorMessage error{common::ErrorCode::authentication_failed};
    const HeartbeatMessage heartbeat{0x0102030405060708ULL};

    auto hello_payload = encode_hello(hello);
    auto ack_payload = encode_hello_ack(ack);
    auto auth_payload = encode_auth(auth);
    auto ok_payload = encode_auth_ok(ok);
    auto error_payload = encode_auth_error(error);
    auto heartbeat_payload = encode_heartbeat(heartbeat);
    ASSERT_TRUE(hello_payload);
    ASSERT_TRUE(ack_payload);
    ASSERT_TRUE(auth_payload);
    ASSERT_TRUE(ok_payload);
    ASSERT_TRUE(error_payload);
    ASSERT_TRUE(heartbeat_payload);

    EXPECT_EQ(*decode_hello(*hello_payload), hello);
    EXPECT_EQ(*decode_hello_ack(*ack_payload), ack);
    EXPECT_EQ(*decode_auth(*auth_payload), auth);
    EXPECT_EQ(*decode_auth_ok(*ok_payload), ok);
    EXPECT_EQ(*decode_auth_error(*error_payload), error);
    EXPECT_EQ(*decode_heartbeat(*heartbeat_payload), heartbeat);
}

TEST(RemoteMessagesTest, RejectsInvalidIdentifiersFixedLengthsAndTrailingBytes) {
    const auto invalid_hello = encode_hello({"client_not-an-id"});
    ASSERT_FALSE(invalid_hello);
    EXPECT_EQ(invalid_hello.error().code(), common::ErrorCode::protocol_error);

    const HelloAckMessage ack{generated_id(common::IdKind::server), 0, {}};
    auto ack_payload = encode_hello_ack(ack);
    ASSERT_TRUE(ack_payload);
    ASSERT_GT(ack_payload->size(), kAuthenticationNonceSize);
    (*ack_payload)[ack_payload->size() - kAuthenticationNonceSize - 2U] = 0U;
    (*ack_payload)[ack_payload->size() - kAuthenticationNonceSize - 1U] = 31U;
    EXPECT_FALSE(decode_hello_ack(*ack_payload));

    auto heartbeat = encode_heartbeat({1U});
    ASSERT_TRUE(heartbeat);
    heartbeat->push_back(0U);
    const auto trailing = decode_heartbeat(*heartbeat);
    ASSERT_FALSE(trailing);
    EXPECT_EQ(trailing.error().code(), common::ErrorCode::protocol_error);
}

TEST(RemoteMessagesTest, EnforcesAuthOkBoundsAndFailureCodes) {
    EXPECT_FALSE(encode_auth_ok({0U, 5'000U, 2U, 32U}));
    EXPECT_FALSE(encode_auth_ok({1U, 999U, 2U, 32U}));
    EXPECT_FALSE(encode_auth_ok({1U, 5'000U, 33U, 32U}));
    EXPECT_FALSE(encode_auth_ok({1U, 5'000U, 2U, 129U}));
    EXPECT_FALSE(encode_auth_error({common::ErrorCode::ok}));
}

} // namespace
} // namespace minitun::protocol
