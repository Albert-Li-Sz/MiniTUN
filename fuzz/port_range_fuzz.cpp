#include <cstddef>
#include <cstdint>
#include <string_view>

#include <minitun/common/port_range.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    auto range = minitun::common::PortRange::parse(input);
    if (range) {
        const std::string canonical = range->to_string();
        static_cast<void>(minitun::common::PortRange::parse(canonical));
    }
    return 0;
}
