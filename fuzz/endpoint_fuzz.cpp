#include <cstddef>
#include <cstdint>
#include <string_view>

#include <minitun/common/endpoint.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    auto endpoint = minitun::common::Endpoint::parse(input);
    if (endpoint) {
        const std::string canonical = endpoint->to_string();
        static_cast<void>(minitun::common::Endpoint::parse(canonical));
    }
    return 0;
}
