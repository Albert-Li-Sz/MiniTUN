#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <minitun/protocol/codec.hpp>
#include <minitun/protocol/frame.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    minitun::protocol::FrameDecoder decoder;
    std::size_t offset = 0U;
    while (offset < size) {
        const std::size_t requested = 1U + static_cast<std::size_t>(data[offset] % 31U);
        const std::size_t chunk = std::min(requested, size - offset);
        auto decoded = decoder.feed(std::span<const std::uint8_t>{data + offset, chunk});
        if (!decoded) {
            decoder.reset();
        } else {
            for (const auto& frame : *decoded) {
                static_cast<void>(minitun::protocol::encode_frame(frame));
                minitun::protocol::PayloadReader reader{frame.payload};
                static_cast<void>(reader.read_u64());
                static_cast<void>(reader.read_string(minitun::protocol::kMaxPayloadSize));
            }
        }
        offset += chunk;
    }
    static_cast<void>(decoder.finish());
    return 0;
}
