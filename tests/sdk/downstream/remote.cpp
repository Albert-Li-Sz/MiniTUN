#include <minitun/remote_protocol.hpp>

int main() {
    minitun::remote::Decoder decoder;
    const std::uint8_t invalid[]{0xffU};
    if (!decoder.feed(invalid)) {
        return 1;
    }
    const auto result = decoder.finish();
    if (result || result.error().code() != minitun::common::ErrorCode::protocol_error ||
        result.error().message().empty()) {
        return 1;
    }
    return 0;
}
