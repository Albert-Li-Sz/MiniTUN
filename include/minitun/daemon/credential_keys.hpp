#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <minitun/common/error.hpp>
#include <minitun/common/id.hpp>
#include <minitun/common/result.hpp>
#include <minitun/storage/credential_store.hpp>
#include <minitun/storage/models.hpp>

namespace minitun::daemon {

enum class ServerCredentialKind : std::uint8_t {
    psk,
    ca_certificate,
    client_certificate,
    client_private_key,
};

[[nodiscard]] inline std::array<std::string, 2>
managed_server_credential_keys(const common::Id& server_id, const ServerCredentialKind kind) {
    std::string primary{"server/"};
    primary.append(server_id.str());
    switch (kind) {
    case ServerCredentialKind::psk:
        break;
    case ServerCredentialKind::ca_certificate:
        primary.append("/ca");
        break;
    case ServerCredentialKind::client_certificate:
        primary.append("/client-cert");
        break;
    case ServerCredentialKind::client_private_key:
        primary.append("/client-key");
        break;
    }
    std::string secondary = primary;
    secondary.append("/next");
    return {std::move(primary), std::move(secondary)};
}

/// Returns the two bounded credential slots managed for one remote server.
///
/// Login writes the inactive slot first and only switches credential_ref after
/// the state transaction commits. Alternating slots prevents a failed state
/// commit from destroying the credential referenced by the current state row.
[[nodiscard]] inline std::array<std::string, 2>
managed_credential_keys(const common::Id& server_id) {
    return managed_server_credential_keys(server_id, ServerCredentialKind::psk);
}

[[nodiscard]] inline const std::optional<std::string>&
credential_reference(const storage::ServerRecord& server, const ServerCredentialKind kind) noexcept {
    switch (kind) {
    case ServerCredentialKind::psk:
        return server.credential_ref;
    case ServerCredentialKind::ca_certificate:
        return server.ca_credential_ref;
    case ServerCredentialKind::client_certificate:
        return server.client_certificate_ref;
    case ServerCredentialKind::client_private_key:
        return server.client_private_key_ref;
    }
    return server.credential_ref;
}

[[nodiscard]] inline std::string next_server_credential_key(
    const storage::ServerRecord& server, const ServerCredentialKind kind) {
    auto keys = managed_server_credential_keys(server.id, kind);
    const auto& current = credential_reference(server, kind);
    if (current.has_value() && *current == keys[0]) {
        return std::move(keys[1]);
    }
    return std::move(keys[0]);
}

[[nodiscard]] inline std::string next_credential_key(const storage::ServerRecord& server) {
    return next_server_credential_key(server, ServerCredentialKind::psk);
}

/// Removes every credential that can belong to one server, except an optional
/// retained key. Historical references and both managed rotation slots are
/// deduplicated. All candidates are attempted so one backend failure cannot
/// leave unrelated orphaned keys behind.
[[nodiscard]] inline common::Result<void>
cleanup_server_credential_kind(storage::CredentialStore& credentials,
                               const common::Id& server_id, const ServerCredentialKind kind,
                               const std::optional<std::string_view> referenced_key = std::nullopt,
                               const std::optional<std::string_view> retained_key = std::nullopt) {
    if (server_id.kind() != common::IdKind::server) {
        return common::Error{common::ErrorCode::invalid_argument,
                             "credential cleanup requires a server ID"};
    }

    try {
        std::vector<std::string> candidates;
        candidates.reserve(3U);
        const auto add_candidate = [&candidates](const std::string_view key) {
            if (std::find(candidates.begin(), candidates.end(), key) == candidates.end()) {
                candidates.emplace_back(key);
            }
        };
        if (referenced_key.has_value()) {
            add_candidate(*referenced_key);
        }
        for (const auto& key : managed_server_credential_keys(server_id, kind)) {
            add_candidate(key);
        }

        std::optional<common::Error> first_error;
        for (const auto& key : candidates) {
            if (retained_key.has_value() && *retained_key == key) {
                continue;
            }
            auto existing = credentials.get(key);
            if (!existing) {
                if (existing.error().code() != common::ErrorCode::not_found &&
                    !first_error.has_value()) {
                    first_error = existing.error();
                }
                continue;
            }
            auto removed = credentials.remove(key);
            if (!removed && !first_error.has_value()) {
                first_error = removed.error();
            }
        }
        return first_error.has_value() ? common::Result<void>::failure(*first_error)
                                       : common::Result<void>::success();
    } catch (const std::bad_alloc&) {
        return common::Error{common::ErrorCode::resource_exhausted,
                             "insufficient memory while cleaning server credentials"};
    }
}

[[nodiscard]] inline common::Result<void>
cleanup_server_credentials(storage::CredentialStore& credentials, const common::Id& server_id,
                           const std::optional<std::string_view> referenced_key = std::nullopt,
                           const std::optional<std::string_view> retained_key = std::nullopt) {
    return cleanup_server_credential_kind(credentials, server_id, ServerCredentialKind::psk,
                                          referenced_key, retained_key);
}

[[nodiscard]] inline common::Result<void>
cleanup_all_server_credentials(storage::CredentialStore& credentials,
                               const storage::ServerRecord& server) {
    std::optional<common::Error> first_error;
    for (const auto kind : {ServerCredentialKind::psk, ServerCredentialKind::ca_certificate,
                            ServerCredentialKind::client_certificate,
                            ServerCredentialKind::client_private_key}) {
        const auto& reference = credential_reference(server, kind);
        auto removed = cleanup_server_credential_kind(
            credentials, server.id, kind,
            reference.has_value() ? std::optional<std::string_view>{*reference} : std::nullopt);
        if (!removed && !first_error.has_value()) {
            first_error = removed.error();
        }
    }
    return first_error.has_value() ? common::Result<void>::failure(*first_error)
                                   : common::Result<void>::success();
}

} // namespace minitun::daemon
