#pragma once

#include <string_view>

#include <minitun/common/result.hpp>
#include <minitun/ipc/protocol.hpp>

namespace minitun::storage {
class CredentialStore;
class StateRepository;
} // namespace minitun::storage

namespace minitun::daemon {

/// Strict format_version=1 declarative state planner and applier.
///
/// Configuration and referenced credential files are fully parsed and
/// validated before apply stages credentials or starts the single state
/// transaction. Credential paths and contents are never returned.
class DeclarativeConfig final {
  public:
    DeclarativeConfig(storage::StateRepository& repository,
                      storage::CredentialStore& credentials) noexcept;

    [[nodiscard]] common::Result<ipc::Json> export_config() const;
    [[nodiscard]] common::Result<ipc::Json> plan(std::string_view path, bool prune) const;
    [[nodiscard]] common::Result<ipc::Json> apply(std::string_view path, bool prune);

  private:
    storage::StateRepository& repository_;
    storage::CredentialStore& credentials_;
};

} // namespace minitun::daemon
