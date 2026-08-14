#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <minitun/common/result.hpp>
#include <minitun/protocol/messages.hpp>

namespace minitun::protocol {

[[nodiscard]] common::Result<AuthenticationNonce> generate_authentication_nonce();
[[nodiscard]] common::Result<std::uint64_t> generate_session_generation();

[[nodiscard]] common::Result<AuthenticationData>
compute_authentication_data(std::string_view token, std::string_view client_id,
                            std::string_view server_id, std::int64_t timestamp_seconds,
                            const AuthenticationNonce& nonce,
                            CapabilitySet selected_capabilities);

[[nodiscard]] common::Result<bool>
verify_authentication_data(std::string_view token, std::string_view client_id,
                           std::string_view server_id, std::int64_t timestamp_seconds,
                           const AuthenticationNonce& nonce,
                           CapabilitySet selected_capabilities,
                           const AuthenticationData& candidate);

[[nodiscard]] common::Result<AuthenticationData>
compute_worker_authentication_data(std::string_view token, std::string_view client_id,
                                   std::string_view server_id, std::uint64_t session_generation,
                                   std::string_view worker_id, std::int64_t timestamp_seconds,
                                   const AuthenticationNonce& nonce);

[[nodiscard]] common::Result<bool>
verify_worker_authentication_data(std::string_view token, std::string_view client_id,
                                  std::string_view server_id, std::uint64_t session_generation,
                                  std::string_view worker_id, std::int64_t timestamp_seconds,
                                  const AuthenticationNonce& nonce,
                                  const AuthenticationData& candidate);

struct NonceReplayCacheOptions final {
    std::size_t max_entries{16'384U};
    std::chrono::seconds retention{std::chrono::minutes{5}};
};

class NonceReplayCache final {
  public:
    explicit NonceReplayCache(NonceReplayCacheOptions options = {});

    [[nodiscard]] common::Result<bool>
    consume(std::string_view client_id, const AuthenticationNonce& nonce,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    [[nodiscard]] std::size_t size() const;

  private:
    void remove_expired(std::chrono::steady_clock::time_point now);

    NonceReplayCacheOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> entries_;
};

[[nodiscard]] common::Result<bool> verify_and_consume_authentication_data(
    NonceReplayCache& replay_cache, std::string_view token, std::string_view client_id,
    std::string_view server_id, std::int64_t timestamp_seconds,
    const AuthenticationNonce& nonce, CapabilitySet selected_capabilities,
    const AuthenticationData& candidate,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

[[nodiscard]] common::Result<bool> verify_and_consume_worker_authentication_data(
    NonceReplayCache& replay_cache, std::string_view token, std::string_view client_id,
    std::string_view server_id, std::uint64_t session_generation, std::string_view worker_id,
    std::int64_t timestamp_seconds, const AuthenticationNonce& nonce,
    const AuthenticationData& candidate,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

struct AuthRateLimiterOptions final {
    std::size_t max_entries{4'096U};
    std::size_t max_failures{5U};
    std::chrono::seconds failure_window{std::chrono::minutes{1}};
    std::chrono::seconds block_duration{std::chrono::minutes{1}};
};

class AuthRateLimiter final {
  public:
    explicit AuthRateLimiter(AuthRateLimiterOptions options = {});

    [[nodiscard]] bool
    allowed(std::string_view key,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    void record_failure(
        std::string_view key,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void record_success(std::string_view key);

    [[nodiscard]] std::size_t size() const;

  private:
    struct Entry final {
        std::deque<std::chrono::steady_clock::time_point> failures;
        std::chrono::steady_clock::time_point blocked_until{};
        std::chrono::steady_clock::time_point last_seen{};
    };

    void prune_failures(Entry& entry, std::chrono::steady_clock::time_point now) const;
    void evict_one();

    AuthRateLimiterOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace minitun::protocol
