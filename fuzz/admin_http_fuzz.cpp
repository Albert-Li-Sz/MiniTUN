#include <cstddef>
#include <cstdint>
#include <string_view>

#include <minitun/admin/server.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    static_cast<void>(minitun::admin::parse_http_request(text));
    return 0;
}
