#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include <minitun/ipc/frame.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    minitun::ipc::FrameDecoder decoder;
    std::size_t offset = 0U;
    while (offset < size) {
        const std::size_t requested = 1U + static_cast<std::size_t>(data[offset] % 63U);
        const std::size_t chunk = std::min(requested, size - offset);
        auto decoded = decoder.feed(std::span<const std::uint8_t>{data + offset, chunk});
        if (!decoded) {
            decoder.reset();
        } else {
            for (const auto& payload : *decoded) {
                static_cast<void>(minitun::ipc::encode_frame(payload));
            }
        }
        offset += chunk;
    }
    static_cast<void>(decoder.finish());
    return 0;
}
