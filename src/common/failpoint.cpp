#include <minitun/common/failpoint.hpp>

#if defined(MINITUN_ENABLE_FAULT_INJECTION)
#include <cstdlib>
#include <string_view>
#endif

namespace minitun::common {

void trigger_failpoint(const std::string_view name) noexcept {
#if defined(MINITUN_ENABLE_FAULT_INJECTION)
    const char* selected = std::getenv("MINITUN_TEST_FAILPOINT");
    if (selected != nullptr && std::string_view{selected} == name) {
        std::_Exit(86);
    }
#else
    static_cast<void>(name);
#endif
}

} // namespace minitun::common
