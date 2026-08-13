#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/protocol/auth.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::string client_id() {
    auto id = common::Id::generate(common::IdKind::client);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

[[nodiscard]] std::string server_id() {
    auto id = common::Id::generate(common::IdKind::server);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

[[nodiscard]] std::string worker_id() {
    auto id = common::Id::generate(common::IdKind::connection);
    EXPECT_TRUE(id) << id.error();
    return id ? id->str() : std::string{};
}

TEST(RemoteAuthTest, ComputesDeterministicHmacAndDetectsEveryChangedInput) {
    AuthenticationNonce nonce{};
    nonce.fill(0x5aU);
    const std::string id = client_id();
    const std::string server = server_id();
    const auto first = compute_authentication_data("correct horse", id, server, 123456, nonce,
                                                   kRequiredCapabilities);
    const auto second = compute_authentication_data("correct horse", id, server, 123456, nonce,
                                                    kRequiredCapabilities);
    ASSERT_TRUE(first) << first.error();
    ASSERT_TRUE(second) << second.error();
    EXPECT_EQ(*first, *second);

    EXPECT_EQ(*verify_authentication_data("correct horse", id, server, 123456, nonce,
                                          kRequiredCapabilities, *first),
              true);
    EXPECT_EQ(*verify_authentication_data("wrong horse", id, server, 123456, nonce,
                                          kRequiredCapabilities, *first),
              false);
    EXPECT_EQ(*verify_authentication_data("correct horse", id, server, 123457, nonce,
                                          kRequiredCapabilities, *first),
              false);
    EXPECT_EQ(*verify_authentication_data("correct horse", id, server_id(), 123456, nonce,
                                          kRequiredCapabilities, *first),
              false);
    EXPECT_EQ(*verify_authentication_data("correct horse", id, server, 123456, nonce,
                                          kSupportedCapabilities, *first),
              false);
    ++nonce[0];
    EXPECT_EQ(*verify_authentication_data("correct horse", id, server, 123456, nonce,
                                          kRequiredCapabilities, *first),
              false);
}

TEST(RemoteAuthTest, RejectsEmptyTokenAndInvalidClientIdentifier) {
    AuthenticationNonce nonce{};
    const auto empty =
        compute_authentication_data("", client_id(), server_id(), 0, nonce, kRequiredCapabilities);
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), common::ErrorCode::invalid_argument);

    const auto invalid = compute_authentication_data("token", "not-a-client", server_id(), 0, nonce,
                                                     kRequiredCapabilities);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), common::ErrorCode::invalid_argument);
}

TEST(RemoteAuthTest, RejectsOversizedTokensAndInvalidCapabilitySelections) {
    AuthenticationNonce nonce{};
    const std::string client = client_id();
    const std::string server = server_id();
    const std::string worker = worker_id();
    const std::string oversized(64U * 1024U + 1U, 'x');

    EXPECT_FALSE(compute_authentication_data(oversized, client, server, 0, nonce,
                                             kRequiredCapabilities));
    EXPECT_FALSE(compute_worker_authentication_data(oversized, client, server, 42U, worker, 0,
                                                    nonce));
    EXPECT_FALSE(compute_authentication_data("token", client, server, 0, nonce,
                                             kRequiredCapabilities | (1ULL << 63U)));
    EXPECT_FALSE(compute_worker_authentication_data("token", client, server, 0U, worker, 0,
                                                    nonce));
}

TEST(RemoteAuthTest, WorkerProofBindsSessionServerIdentityAndRejectsReplay) {
    const std::string client = client_id();
    const std::string server = server_id();
    const std::string worker = worker_id();
    AuthenticationNonce nonce{};
    nonce.fill(0x42U);
    auto proof = compute_worker_authentication_data("worker-secret", client, server, 42U, worker,
                                                    123456, nonce);
    ASSERT_TRUE(proof) << proof.error();
    EXPECT_TRUE(*verify_worker_authentication_data("worker-secret", client, server, 42U, worker,
                                                   123456, nonce, *proof));
    EXPECT_FALSE(*verify_worker_authentication_data("wrong-secret", client, server, 42U, worker,
                                                    123456, nonce, *proof));
    EXPECT_FALSE(*verify_worker_authentication_data("worker-secret", client, server, 43U, worker,
                                                    123456, nonce, *proof));
    EXPECT_FALSE(*verify_worker_authentication_data("worker-secret", client, server, 42U,
                                                    worker_id(), 123456, nonce, *proof));
    EXPECT_FALSE(*verify_worker_authentication_data("worker-secret", client, server_id(), 42U,
                                                    worker, 123456, nonce, *proof));

    NonceReplayCache cache;
    const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
    auto accepted = verify_and_consume_worker_authentication_data(
        cache, "worker-secret", client, server, 42U, worker, 123456, nonce, *proof, now);
    ASSERT_TRUE(accepted) << accepted.error();
    EXPECT_TRUE(*accepted);
    auto replay = verify_and_consume_worker_authentication_data(
        cache, "worker-secret", client, server, 42U, worker, 123456, nonce, *proof, now);
    ASSERT_TRUE(replay) << replay.error();
    EXPECT_FALSE(*replay);
}

TEST(RemoteAuthTest, GeneratesNonZeroIndependentCryptographicValues) {
    const auto first_nonce = generate_authentication_nonce();
    const auto second_nonce = generate_authentication_nonce();
    const auto first_generation = generate_session_generation();
    const auto second_generation = generate_session_generation();
    ASSERT_TRUE(first_nonce);
    ASSERT_TRUE(second_nonce);
    ASSERT_TRUE(first_generation);
    ASSERT_TRUE(second_generation);
    EXPECT_NE(*first_nonce, *second_nonce);
    EXPECT_NE(*first_generation, 0U);
    EXPECT_NE(*second_generation, 0U);
    EXPECT_NE(*first_generation, *second_generation);
}

TEST(RemoteAuthTest, ReplayCacheRejectsDuplicatesExpiresAndStaysBounded) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{std::chrono::seconds{100}};
    NonceReplayCache cache{{.max_entries = 2U, .retention = std::chrono::seconds{10}}};
    const std::string id = client_id();
    const std::string other_id = client_id();
    AuthenticationNonce first{};
    AuthenticationNonce second{};
    second[0] = 1U;

    EXPECT_EQ(*cache.consume(id, first, now), true);
    EXPECT_EQ(*cache.consume(id, first, now + std::chrono::seconds{1}), false);
    // The same nonce from a different client must not collide.
    EXPECT_EQ(*cache.consume(other_id, first, now + std::chrono::seconds{1}), true);
    const auto full = cache.consume(id, second, now + std::chrono::seconds{1});
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code(), common::ErrorCode::resource_exhausted);
    EXPECT_EQ(cache.size(), 2U);
    EXPECT_EQ(*cache.consume(id, second, now + std::chrono::seconds{11}), true);
}

TEST(RemoteAuthTest, FailedVerificationDoesNotConsumeNonce) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{std::chrono::seconds{100}};
    const std::string id = client_id();
    const std::string server = server_id();
    AuthenticationNonce nonce{};
    nonce.fill(0x5aU);
    const auto bad = compute_authentication_data("wrong token", id, server, 123456, nonce,
                                                 kRequiredCapabilities);
    const auto good = compute_authentication_data("correct token", id, server, 123456, nonce,
                                                  kRequiredCapabilities);
    ASSERT_TRUE(bad) << bad.error();
    ASSERT_TRUE(good) << good.error();

    NonceReplayCache cache{};
    const auto rejected = verify_and_consume_authentication_data(
        cache, "correct token", id, server, 123456, nonce, kRequiredCapabilities, *bad, now);
    ASSERT_TRUE(rejected) << rejected.error();
    EXPECT_FALSE(*rejected);
    EXPECT_EQ(cache.size(), 0U);

    const auto accepted = verify_and_consume_authentication_data(
        cache, "correct token", id, server, 123456, nonce, kRequiredCapabilities, *good, now);
    ASSERT_TRUE(accepted) << accepted.error();
    EXPECT_TRUE(*accepted);
    EXPECT_EQ(cache.size(), 1U);
}

TEST(RemoteAuthTest, RateLimiterBlocksAtThresholdAndRecovers) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{std::chrono::seconds{100}};
    AuthRateLimiter limiter{{
        .max_entries = 2U,
        .max_failures = 3U,
        .failure_window = std::chrono::seconds{10},
        .block_duration = std::chrono::seconds{20},
    }};

    EXPECT_TRUE(limiter.allowed("192.0.2.1", now));
    limiter.record_failure("192.0.2.1", now);
    limiter.record_failure("192.0.2.1", now + std::chrono::seconds{1});
    EXPECT_TRUE(limiter.allowed("192.0.2.1", now + std::chrono::seconds{2}));
    limiter.record_failure("192.0.2.1", now + std::chrono::seconds{2});
    EXPECT_FALSE(limiter.allowed("192.0.2.1", now + std::chrono::seconds{3}));
    EXPECT_TRUE(limiter.allowed("192.0.2.1", now + std::chrono::seconds{23}));
    limiter.record_success("192.0.2.1");
    EXPECT_EQ(limiter.size(), 0U);
}

TEST(RemoteAuthTest, RejectsEveryControlAuthenticationInputInvariant) {
    AuthenticationNonce nonce{};
    const std::string client = client_id();
    const std::string server = server_id();
    const std::string oversized_token(64U * 1'024U + 1U, 'p');
    const auto expect_invalid = [&nonce](const std::string_view token,
                                         const std::string_view candidate_client,
                                         const std::string_view candidate_server,
                                         const CapabilitySet capabilities) {
        const auto result = compute_authentication_data(token, candidate_client, candidate_server,
                                                        123, nonce, capabilities);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    };

    expect_invalid("", client, server, kRequiredCapabilities);
    expect_invalid(oversized_token, client, server, kRequiredCapabilities);
    expect_invalid("token", "invalid-client", server, kRequiredCapabilities);
    expect_invalid("token", client, "invalid-server", kRequiredCapabilities);
    expect_invalid("token", client, server, 0U);
    expect_invalid("token", client, server, kSupportedCapabilities | (CapabilitySet{1U} << 63U));

    AuthenticationData candidate{};
    const auto propagated = verify_authentication_data("", client, server, 123, nonce,
                                                       kRequiredCapabilities, candidate);
    ASSERT_FALSE(propagated);
    EXPECT_EQ(propagated.error().code(), common::ErrorCode::invalid_argument);
}

TEST(RemoteAuthTest, RejectsEveryWorkerAuthenticationInputInvariant) {
    AuthenticationNonce nonce{};
    const std::string client = client_id();
    const std::string server = server_id();
    const std::string worker = worker_id();
    const std::string oversized_token(64U * 1'024U + 1U, 'p');
    const auto expect_invalid = [&nonce](const std::string_view token,
                                         const std::string_view candidate_client,
                                         const std::string_view candidate_server,
                                         const std::uint64_t generation,
                                         const std::string_view candidate_worker) {
        const auto result = compute_worker_authentication_data(
            token, candidate_client, candidate_server, generation, candidate_worker, 123, nonce);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    };

    expect_invalid("", client, server, 1U, worker);
    expect_invalid(oversized_token, client, server, 1U, worker);
    expect_invalid("token", "invalid-client", server, 1U, worker);
    expect_invalid("token", client, "invalid-server", 1U, worker);
    expect_invalid("token", client, server, 0U, worker);
    expect_invalid("token", client, server, 1U, "invalid-worker");

    AuthenticationData candidate{};
    const auto propagated =
        verify_worker_authentication_data("", client, server, 1U, worker, 123, nonce, candidate);
    ASSERT_FALSE(propagated);
    EXPECT_EQ(propagated.error().code(), common::ErrorCode::invalid_argument);

    NonceReplayCache cache;
    const auto consumed = verify_and_consume_worker_authentication_data(
        cache, "", client, server, 1U, worker, 123, nonce, candidate,
        std::chrono::steady_clock::time_point{});
    ASSERT_FALSE(consumed);
    EXPECT_EQ(consumed.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(cache.size(), 0U);
}

TEST(RemoteAuthTest, NormalizesReplayCacheOptionsAndRejectsInvalidClient) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{std::chrono::seconds{100}};
    NonceReplayCache cache{{.max_entries = 0U, .retention = std::chrono::seconds::zero()}};
    AuthenticationNonce first{};
    AuthenticationNonce second{};
    second[0] = 1U;

    const auto invalid = cache.consume("invalid-client", first, now);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_TRUE(*cache.consume(client_id(), first, now));
    const auto full = cache.consume(client_id(), second, now);
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code(), common::ErrorCode::resource_exhausted);
    EXPECT_TRUE(*cache.consume(client_id(), second, now + std::chrono::seconds{1}));
    EXPECT_EQ(cache.size(), 1U);
}

TEST(RemoteAuthTest, InvalidVerifiedControlProofDoesNotReachReplayCache) {
    AuthenticationNonce nonce{};
    AuthenticationData candidate{};
    NonceReplayCache cache;
    const auto result = verify_and_consume_authentication_data(
        cache, "", client_id(), server_id(), 123, nonce, kRequiredCapabilities, candidate,
        std::chrono::steady_clock::time_point{});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), common::ErrorCode::invalid_argument);
    EXPECT_EQ(cache.size(), 0U);
}

TEST(RemoteAuthTest, RateLimiterNormalizesOptionsPrunesAndEvictsLeastRecentEntry) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{std::chrono::seconds{100}};

    AuthRateLimiter normalized{{.max_entries = 0U,
                                .max_failures = 0U,
                                .failure_window = std::chrono::seconds::zero(),
                                .block_duration = std::chrono::seconds::zero()}};
    normalized.record_failure("only", now);
    EXPECT_FALSE(normalized.allowed("only", now));
    EXPECT_TRUE(normalized.allowed("only", now + std::chrono::seconds{1}));
    normalized.record_failure("replacement", now + std::chrono::seconds{2});
    EXPECT_EQ(normalized.size(), 1U);

    AuthRateLimiter pruning{{.max_entries = 4U,
                             .max_failures = 2U,
                             .failure_window = std::chrono::seconds{5},
                             .block_duration = std::chrono::seconds{10}}};
    pruning.record_failure("client", now);
    pruning.record_failure("client", now + std::chrono::seconds{6});
    EXPECT_TRUE(pruning.allowed("client", now + std::chrono::seconds{6}));

    AuthRateLimiter eviction{{.max_entries = 2U,
                              .max_failures = 1U,
                              .failure_window = std::chrono::seconds{30},
                              .block_duration = std::chrono::seconds{30}}};
    eviction.record_failure("oldest", now);
    eviction.record_failure("newer", now + std::chrono::seconds{1});
    EXPECT_FALSE(eviction.allowed("oldest", now + std::chrono::seconds{2}));
    eviction.record_failure("third", now + std::chrono::seconds{3});
    EXPECT_EQ(eviction.size(), 2U);
    EXPECT_TRUE(eviction.allowed("newer", now + std::chrono::seconds{4}));
    EXPECT_FALSE(eviction.allowed("oldest", now + std::chrono::seconds{4}));
    EXPECT_FALSE(eviction.allowed("third", now + std::chrono::seconds{4}));
    eviction.record_success("missing");
    EXPECT_EQ(eviction.size(), 2U);
}

} // namespace
} // namespace minitun::protocol
