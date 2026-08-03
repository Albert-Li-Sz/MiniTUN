#pragma once

#include <array>
#include <string>
#include <utility>

#include <minitun/common/id.hpp>
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

} // namespace minitun::daemon
