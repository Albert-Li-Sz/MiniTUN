#include <cstddef>
#include <cstdint>
#include <string_view>

#include <minitun/ipc/protocol.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    const std::string_view payload{reinterpret_cast<const char*>(data), size};
    auto request = minitun::ipc::parse_request(payload);
    if (request) {
        static_cast<void>(minitun::ipc::serialize_request(*request));
    }
    auto response = minitun::ipc::parse_response(payload);
    if (response) {
        static_cast<void>(minitun::ipc::serialize_response(*response));
    }
    return 0;
}
