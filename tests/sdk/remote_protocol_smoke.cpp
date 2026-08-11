#include <array>
#include <cstdint>

#include <minitun/remote_protocol.hpp>

int main() {
    minitun::remote::Decoder decoder;
    const std::array<std::uint8_t, 0U> empty{};
    const auto decoded = decoder.feed(empty);
    if (!decoded || !decoded->empty() || !decoder.finish()) {
        return 1;
    }
    const minitun::remote::Message message{minitun::protocol::HeartbeatMessage{9U}};
    const auto frame =
        minitun::remote::Codec::make_frame(minitun::protocol::MessageType::ping, 9U, message);
    return frame ? 0 : 2;
}
