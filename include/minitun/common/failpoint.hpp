#pragma once

#include <string_view>

namespace minitun::common {

/// Terminates with status 86 when fault injection is compiled in and the
/// MINITUN_TEST_FAILPOINT environment variable exactly matches `name`.
/// Package and release builds leave this as an unconditional no-op.
void trigger_failpoint(std::string_view name) noexcept;

} // namespace minitun::common
