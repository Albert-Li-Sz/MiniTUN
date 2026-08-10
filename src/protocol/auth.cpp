#include <minitun/protocol/auth.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <minitun/common/id.hpp>
#include <minitun/protocol/codec.hpp>

namespace minitun::protocol {
namespace {

[[nodiscard]] std::string replay_key(const std::string_view client_id,
                                     const AuthenticationNonce& nonce) {
    std::string key;
    key.reserve(client_id.size() + 1U + nonce.size());
    key.append(client_id);
    key.push_back(':');
    key.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    return key;
}

} // namespace

common::Result<AuthenticationNonce> generate_authentication_nonce() {
    AuthenticationNonce nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return common::Result<AuthenticationNonce>::failure(
            common::ErrorCode::internal_error,
            "cryptographic nonce generation failed");
    }
    return nonce;
}

common::Result<std::uint64_t> generate_session_generation() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::array<std::uint8_t, sizeof(std::uint64_t)> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            return common::Result<std::uint64_t>::failure(
                common::ErrorCode::internal_error,
                "session generation randomness failed");
        }

        std::uint64_t value = 0U;
        for (const std::uint8_t byte : bytes) {
            value = (value << 8U) | static_cast<std::uint64_t>(byte);
        }
        if (value != 0U) {
            return value;
        }
    }
    return common::Result<std::uint64_t>::failure(
        common::ErrorCode::internal_error,
        "session generation randomness produced an invalid value");
}

common::Result<AuthenticationData>
compute_authentication_data(const std::string_view token, const std::string_view client_id,
                            const std::string_view server_id,
                            const std::int64_t timestamp_seconds,
                            const AuthenticationNonce& nonce,
                            const CapabilitySet selected_capabilities) {
    if (token.empty() || token.size() > 64U * 1024U) {
        return common::Result<AuthenticationData>::failure(
            common::ErrorCode::invalid_argument,
            "authentication token length is invalid");
    }
    if (!common::Id::parse(client_id, common::IdKind::client)) {
        return common::Result<AuthenticationData>::failure(
            common::ErrorCode::invalid_argument,
            "client ID is invalid for authentication");
    }
    if (!common::Id::parse(server_id, common::IdKind::server)) {
        return common::Result<AuthenticationData>::failure(
            common::ErrorCode::invalid_argument,
            "server ID is invalid for authentication");
    }
    if ((selected_capabilities & kRequiredCapabilities) != kRequiredCapabilities ||
        (selected_capabilities & ~kSupportedCapabilities) != 0U) {
        return common::Result<AuthenticationData>::failure(
            common::ErrorCode::invalid_argument,
            "authentication capability selection is invalid");
    }

    PayloadWriter writer;
    if (auto result = writer.write_u16(kProtocolVersion); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_string(client_id); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_string(server_id); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_u64(std::bit_cast<std::uint64_t>(timestamp_seconds)); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_bytes(nonce); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_u64(selected_capabilities); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    auto input = std::move(writer).finish();
    if (!input) {
        return common::Result<AuthenticationData>::failure(input.error());
    }

    AuthenticationData output{};
    std::size_t output_size = 0U;
    const auto* const result = EVP_Q_mac(
        nullptr, "HMAC", nullptr, "SHA256", nullptr, token.data(), token.size(), input->data(),
        input->size(), output.data(), output.size(), &output_size);
    if (result == nullptr || output_size != output.size()) {
        return common::Result<AuthenticationData>::failure(
            common::ErrorCode::internal_error,
            "authentication digest generation failed");
    }
    return output;
}

common::Result<bool>
verify_authentication_data(const std::string_view token, const std::string_view client_id,
                           const std::string_view server_id,
                           const std::int64_t timestamp_seconds,
                           const AuthenticationNonce& nonce,
                           const CapabilitySet selected_capabilities,
                           const AuthenticationData& candidate) {
    auto expected = compute_authentication_data(token, client_id, server_id,
                                                timestamp_seconds, nonce,
                                                selected_capabilities);
    if (!expected) {
        return common::Result<bool>::failure(expected.error());
    }
    return CRYPTO_memcmp(expected->data(), candidate.data(), expected->size()) == 0;
}

common::Result<AuthenticationData>
compute_worker_authentication_data(const std::string_view token,
                                   const std::string_view client_id,
                                   const std::string_view server_id,
                                   const std::uint64_t session_generation,
                                   const std::string_view worker_id,
                                   const std::int64_t timestamp_seconds,
                                   const AuthenticationNonce& nonce) {
    if (token.empty() || token.size() > 64U * 1024U) {
        return common::Result<AuthenticationData>::failure(
            common::ErrorCode::invalid_argument, "worker authentication token length is invalid");
    }
    if (!common::Id::parse(client_id, common::IdKind::client) ||
        !common::Id::parse(server_id, common::IdKind::server) ||
        !common::Id::parse(worker_id, common::IdKind::connection) ||
        session_generation == 0U) {
        return common::Result<AuthenticationData>::failure(
            common::ErrorCode::invalid_argument, "worker authentication identity is invalid");
    }

    PayloadWriter writer;
    if (auto result = writer.write_string("minitun-worker-auth"); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_u16(kProtocolVersion); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_string(client_id); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_string(server_id); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_u64(session_generation); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_string(worker_id); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_u64(std::bit_cast<std::uint64_t>(timestamp_seconds)); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    if (auto result = writer.write_bytes(nonce); !result) {
        return common::Result<AuthenticationData>::failure(result.error());
    }
    auto input = std::move(writer).finish();
    if (!input) {
        return common::Result<AuthenticationData>::failure(input.error());
    }

    AuthenticationData output{};
    std::size_t output_size = 0U;
    const auto* const result = EVP_Q_mac(
        nullptr, "HMAC", nullptr, "SHA256", nullptr, token.data(), token.size(), input->data(),
        input->size(), output.data(), output.size(), &output_size);
    if (result == nullptr || output_size != output.size()) {
        return common::Result<AuthenticationData>::failure(
            common::ErrorCode::internal_error, "worker authentication digest generation failed");
    }
    return output;
}

common::Result<bool>
verify_worker_authentication_data(const std::string_view token,
                                  const std::string_view client_id,
                                  const std::string_view server_id,
                                  const std::uint64_t session_generation,
                                  const std::string_view worker_id,
                                  const std::int64_t timestamp_seconds,
                                  const AuthenticationNonce& nonce,
                                  const AuthenticationData& candidate) {
    auto expected = compute_worker_authentication_data(token, client_id, server_id,
                                                       session_generation, worker_id,
                                                       timestamp_seconds, nonce);
    if (!expected) {
        return common::Result<bool>::failure(expected.error());
    }
    return CRYPTO_memcmp(expected->data(), candidate.data(), expected->size()) == 0;
}

NonceReplayCache::NonceReplayCache(NonceReplayCacheOptions options)
    : options_(options) {
    options_.max_entries = std::max<std::size_t>(options_.max_entries, 1U);
    if (options_.retention <= std::chrono::seconds::zero()) {
        options_.retention = std::chrono::seconds{1};
    }
}

common::Result<bool>
NonceReplayCache::consume(const std::string_view client_id, const AuthenticationNonce& nonce,
                          const std::chrono::steady_clock::time_point now) {
    if (!common::Id::parse(client_id, common::IdKind::client)) {
        return common::Result<bool>::failure(
            common::ErrorCode::invalid_argument,
            "authentication replay client ID is invalid");
    }
    std::scoped_lock lock{mutex_};
    remove_expired(now);
    const std::string key = replay_key(client_id, nonce);
    if (entries_.contains(key)) {
        return false;
    }
    if (entries_.size() >= options_.max_entries) {
        return common::Result<bool>::failure(
            common::ErrorCode::resource_exhausted,
            "authentication replay cache reached its configured limit");
    }
    entries_.emplace(key, now + options_.retention);
    return true;
}

std::size_t NonceReplayCache::size() const {
    std::scoped_lock lock{mutex_};
    return entries_.size();
}

void NonceReplayCache::remove_expired(const std::chrono::steady_clock::time_point now) {
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        if (iterator->second <= now) {
            iterator = entries_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

common::Result<bool> verify_and_consume_authentication_data(
    NonceReplayCache& replay_cache, const std::string_view token,
    const std::string_view client_id, const std::string_view server_id,
    const std::int64_t timestamp_seconds, const AuthenticationNonce& nonce,
    const CapabilitySet selected_capabilities, const AuthenticationData& candidate,
    const std::chrono::steady_clock::time_point now) {
    auto verified =
        verify_authentication_data(token, client_id, server_id, timestamp_seconds, nonce,
                                   selected_capabilities, candidate);
    if (!verified || !*verified) {
        return verified;
    }
    return replay_cache.consume(client_id, nonce, now);
}

common::Result<bool> verify_and_consume_worker_authentication_data(
    NonceReplayCache& replay_cache, const std::string_view token,
    const std::string_view client_id, const std::string_view server_id,
    const std::uint64_t session_generation, const std::string_view worker_id,
    const std::int64_t timestamp_seconds, const AuthenticationNonce& nonce,
    const AuthenticationData& candidate, const std::chrono::steady_clock::time_point now) {
    auto verified = verify_worker_authentication_data(
        token, client_id, server_id, session_generation, worker_id, timestamp_seconds, nonce,
        candidate);
    if (!verified || !*verified) {
        return verified;
    }
    return replay_cache.consume(client_id, nonce, now);
}

AuthRateLimiter::AuthRateLimiter(AuthRateLimiterOptions options) : options_(options) {
    options_.max_entries = std::max<std::size_t>(options_.max_entries, 1U);
    options_.max_failures = std::max<std::size_t>(options_.max_failures, 1U);
    if (options_.failure_window <= std::chrono::seconds::zero()) {
        options_.failure_window = std::chrono::seconds{1};
    }
    if (options_.block_duration <= std::chrono::seconds::zero()) {
        options_.block_duration = std::chrono::seconds{1};
    }
}

bool AuthRateLimiter::allowed(const std::string_view key,
                              const std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock{mutex_};
    const auto iterator = entries_.find(std::string{key});
    if (iterator == entries_.end()) {
        return true;
    }
    prune_failures(iterator->second, now);
    iterator->second.last_seen = now;
    return now >= iterator->second.blocked_until;
}

void AuthRateLimiter::record_failure(const std::string_view key,
                                     const std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock{mutex_};
    const std::string owned_key{key};
    if (!entries_.contains(owned_key) && entries_.size() >= options_.max_entries) {
        evict_one();
    }
    Entry& entry = entries_[owned_key];
    prune_failures(entry, now);
    entry.failures.push_back(now);
    entry.last_seen = now;
    if (entry.failures.size() >= options_.max_failures) {
        entry.blocked_until = now + options_.block_duration;
        entry.failures.clear();
    }
}

void AuthRateLimiter::record_success(const std::string_view key) {
    std::scoped_lock lock{mutex_};
    entries_.erase(std::string{key});
}

std::size_t AuthRateLimiter::size() const {
    std::scoped_lock lock{mutex_};
    return entries_.size();
}

void AuthRateLimiter::prune_failures(Entry& entry,
                                     const std::chrono::steady_clock::time_point now) const {
    const auto cutoff = now - options_.failure_window;
    while (!entry.failures.empty() && entry.failures.front() <= cutoff) {
        entry.failures.pop_front();
    }
}

void AuthRateLimiter::evict_one() {
    if (entries_.empty()) {
        return;
    }
    auto oldest = entries_.begin();
    for (auto iterator = std::next(entries_.begin()); iterator != entries_.end(); ++iterator) {
        if (iterator->second.last_seen < oldest->second.last_seen) {
            oldest = iterator;
        }
    }
    entries_.erase(oldest);
}

} // namespace minitun::protocol
