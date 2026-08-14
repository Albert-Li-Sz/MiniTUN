// 本地控制 SDK 示例：连接本机 daemon，列出 server 并检查 daemon 状态。
// 构建：g++ -std=c++20 local_control.cpp -lminitun-client -o local_control
#include <cstdio>

#include <minitun/client.h>

namespace {
void print_error(const minitun_error* error) {
    if (error != nullptr) {
        std::fprintf(stderr, "minitun error %d: %s\n", static_cast<int>(error->code),
                     error->message != nullptr ? error->message : "");
    }
}
} // namespace

int main() {
    minitun_client* client = nullptr;
    minitun_error* error = nullptr;
    if (minitun_client_open(&client, nullptr, &error) != 0 || client == nullptr) {
        std::fprintf(stderr, "failed to open local client (is minitund running?)\n");
        print_error(error);
        return 1;
    }
    minitun_status status{};
    if (minitun_client_status_get(client, &status, &error) != 0) {
        print_error(error);
    } else {
        std::printf("client_id: %s\nservers: %u, tunnels: %u\n",
                    status.client_id != nullptr ? status.client_id : "(unknown)",
                    status.server_total, status.tunnel_total);
    }
    minitun_client_destroy(client);
    return 0;
}
