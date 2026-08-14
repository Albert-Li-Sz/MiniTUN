// Remote Protocol SDK 示例：编码一个注册帧，逐字节增量解码，再解码回消息。
// 构建：g++ -std=c++20 remote_codec.cpp -lminitun-remote-protocol -o remote_codec
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include <minitun/remote_protocol.hpp>

int main() {
    const minitun::protocol::RegisterTunnelMessage registration{
        "tun_00000000000000000000000000000001", "0.0.0.0", 6000U, 1U,
        minitun::protocol::TunnelMode::udp};
    auto frame = minitun::remote::Codec::make_frame(
        minitun::protocol::MessageType::register_tunnel, 7U,
        minitun::remote::Message{registration});
    auto bytes = minitun::remote::Codec::encode_frame(*frame);
    if (!frame || !bytes) {
        std::fprintf(stderr, "encode failed\n");
        return 1;
    }
    std::printf("encoded %zu bytes\n", bytes->size());

    minitun::remote::Decoder decoder;
    std::vector<minitun::protocol::Frame> decoded;
    for (const std::uint8_t byte : *bytes) {
        const std::array<std::uint8_t, 1U> fragment{byte};
        auto parsed = decoder.feed(fragment);
        if (!parsed) {
            std::fprintf(stderr, "decode failed\n");
            return 1;
        }
        decoded.insert(decoded.end(), parsed->begin(), parsed->end());
    }
    auto message = minitun::remote::Codec::decode_message(decoded.at(0U));
    if (!message || !std::holds_alternative<minitun::protocol::RegisterTunnelMessage>(*message)) {
        std::fprintf(stderr, "round trip failed\n");
        return 1;
    }
    const auto& restored = std::get<minitun::protocol::RegisterTunnelMessage>(*message);
    std::printf("round trip ok: %s:%u mode=%u\n", restored.tunnel_id.c_str(),
                restored.bind_port, static_cast<unsigned>(restored.mode));
    return 0;
}
