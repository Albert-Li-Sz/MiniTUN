#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <minitun/admin/server.hpp>
#include <minitun/common/result.hpp>

namespace minitun::server {

struct PolicyManagementOptions final {
    /// Default PSK rotation grace window in seconds (1..86400).
    std::int64_t rotation_grace_seconds{600};
};

/// Access points the management handler needs from the owning server.
struct PolicyManagementBindings final {
    std::function<common::Result<std::string>()> document;
    /// Validates, persists, and activates a replacement policy document.
    std::function<common::Result<std::vector<std::string>>(std::string)> replace;
    std::function<common::Result<std::vector<std::string>>()> reload;
    std::function<std::string()> server_id;
    /// Directory containing the PSK files referenced by the policy document.
    std::function<std::string()> config_directory;
};

/// Builds the admin /v1/* management handler operating on client policies:
/// list/get, upsert, delete, PSK rotation with a grace window, and reload.
/// Every mutation edits a copy of the current document and hands it to the
/// replace binding, which validates and atomically persists it before the
/// change becomes active; PSK secrets are only returned when generated.
[[nodiscard]] common::Result<std::function<common::Result<admin::ManagementResponse>(
    const admin::ManagementRequest&)>>
make_policy_management_handler(PolicyManagementBindings bindings,
                               PolicyManagementOptions options);

} // namespace minitun::server
