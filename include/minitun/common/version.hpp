#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace minitun::common {

struct VersionInfo final {
    std::string_view version;
    std::string_view git_commit;
    std::string_view build_type;
    std::string_view compiler;
    std::uint16_t protocol_version;
};

[[nodiscard]] VersionInfo version_info() noexcept;

/// Formats the complete, human-readable --version output for an executable.
[[nodiscard]] std::string format_version_info(std::string_view program_name);

/// Convenience form used when no executable-specific name is needed.
[[nodiscard]] std::string version_text();

} // namespace minitun::common
