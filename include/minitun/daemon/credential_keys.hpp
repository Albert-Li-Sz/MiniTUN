#pragma once

#include <algorithm>
#include <array>
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

/// Returns the two bounded credential slots managed for one remote server.
///
/// Login writes the inactive slot first and only switches credential_ref after
/// the state transaction commits. Alternating slots prevents a failed state
/// commit from destroying the credential referenced by the current state row.
[[nodiscard]] inline std::array<std::string, 2>
managed_credential_keys(const common::Id& server_id) {
    std::string primary{"server/"};
    primary.append(server_id.str());
    std::string secondary = primary;
    secondary.append("/next");
    return {std::move(primary), std::move(secondary)};
}

[[nodiscard]] inline std::string next_credential_key(const storage::ServerRecord& server) {
    auto keys = managed_credential_keys(server.id);
    if (server.credential_ref.has_value() && *server.credential_ref == keys[0]) {
        return std::move(keys[1]);
    }
    return std::move(keys[0]);
}

/// Removes every credential that can belong to one server, except an optional
/// retained key. Historical references and both managed rotation slots are
/// deduplicated. All candidates are attempted so one backend failure cannot
/// leave unrelated orphaned keys behind.
[[nodiscard]] inline common::Result<void>
cleanup_server_credentials(storage::CredentialStore& credentials, const common::Id& server_id,
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
        for (const auto& key : managed_credential_keys(server_id)) {
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

} // namespace minitun::daemon
