# SDK

当前源码提供两个相互独立、SOVERSION 都为 1 的 SDK：

- `libminitun-client.so.1` 只通过 Unix IPC 控制本机 `minitund`，不嵌入 daemon/server，
  也不暴露 JSON、Asio、SQLite 或内部记录；
- `libminitun-remote-protocol.so.1` 提供 Remote Protocol v2 的强类型 message、增量 frame
  decoder、codec 和认证摘要 helper，不创建 socket、TLS session 或运行时。

## 安装与链接

Debian/Ubuntu 安装 `libminitun-client1` 和 `libminitun-client-dev`；RPM 安装
`libminitun-client1` 和 `libminitun-client-devel`。

CMake（安装包同时导出两个 target）：

```cmake
find_package(MiniTun 1.1 REQUIRED CONFIG)
target_link_libraries(my_tool PRIVATE MiniTun::Client)
target_link_libraries(my_protocol_tool PRIVATE MiniTun::RemoteProtocol)
```

pkg-config：

```bash
cc -std=c11 tool.c $(pkg-config --cflags --libs minitun-client)
c++ -std=c++20 tool.cpp $(pkg-config --cflags --libs minitun-client)
c++ -std=c++20 protocol.cpp \
  $(pkg-config --cflags --libs minitun-remote-protocol)
```

## 本地控制 SDK

## C11 示例

```c
#include <minitun/client.h>
#include <stdio.h>

int main(void) {
    minitun_client *client = NULL;
    minitun_error *error = NULL;
    minitun_client_options options = {sizeof(options), NULL};
    if (minitun_client_abi_version() != MINITUN_CLIENT_ABI_VERSION ||
        minitun_client_create(&options, &client, &error) != 0) {
        fprintf(stderr, "%s\n", error ? error->message : "ABI mismatch");
        minitun_error_free(error);
        return 1;
    }
    minitun_status status = {0};
    if (minitun_client_status_get(client, &status, &error) == 0) {
        printf("active tunnels: %llu\n", (unsigned long long)status.tunnel_active);
    }
    minitun_error_free(error);
    minitun_client_destroy(client);
    return 0;
}
```

所有返回对象都有对应显式释放函数。ABI 边界不抛 C++ 异常；失败返回非零值并可选填充
稳定错误码和不敏感消息。输入结构的 `struct_size` 必须设置为调用方编译时的大小。

## C++20 示例

```cpp
#include <minitun/client.hpp>
#include <iostream>

int main() {
    auto created = minitun::Client::create();
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto status = created.value().status();
    if (!status) {
        std::cerr << status.error().message << '\n';
        return 1;
    }
    std::cout << status.value().tunnel_active << '\n';
}
```

C++ wrapper 提供 RAII handle、强类型模型、`Result<T>`、`UpdateField<T>` 和生命周期动作。
同一 `Client` 对象可由多个线程调用；每个操作独立建立有界本地 IPC 请求。

`TunnelCreate::protocol` 接受 `tcp`、`udp`、`socks5` 或 `p2p`，`remote_host` 控制 server
侧数值 bind address；C API 的 `minitun_tunnel_create_request` 和
`minitun_tunnel_update_request` 在结构尾部提供对应字段。旧调用方较小的 `struct_size`
仍被接受，并按 `tcp`/`0.0.0.0` 解释，因此 1.0 二进制无需重编译即可继续工作。

## Remote Protocol C++20 SDK

头文件为 `<minitun/remote_protocol.hpp>`。下面把一个强类型 `HELLO` 编码成完整 wire
frame，再用可接受任意 TCP/TLS 分片的 decoder 还原：

```cpp
#include <minitun/remote_protocol.hpp>

using namespace minitun;

remote::Message message = protocol::HelloMessage{
    .client_id = "client_0123456789abcdef0123456789abcdef",
    .capabilities = protocol::kSupportedCapabilities,
};
auto frame = remote::Codec::make_frame(protocol::MessageType::hello, 1, message);
if (!frame) {
    return 1;
}
auto wire = remote::Codec::encode_frame(*frame);
if (!wire) {
    return 1;
}

remote::Decoder decoder;
auto frames = decoder.feed(*wire);
if (!frames || frames->size() != 1 || !decoder.finish()) {
    return 1;
}
auto decoded = remote::Codec::decode_message(frames->front());
return decoded ? 0 : 1;
```

`Decoder::feed()` 可以返回零到多个完整帧；EOF 时必须调用 `finish()`，它会拒绝截断的
header/payload。`reset()` 可复用 decoder。`Codec::control_authentication_data()` 与
`worker_authentication_data()` 生成协议规定的 HMAC 输入结果；调用方仍负责 TLS 1.2+、
nonce 随机性/重放缓存、时钟窗口、状态机和秘密生命周期。

## ABI 承诺

两个共享库都默认隐藏符号。本地 SDK 只导出 `minitun_*` 稳定 API；Remote SDK 使用独立
linker export list，只导出 `minitun::remote` 的公共构造、decoder 和 codec 符号。CI
检查两套动态符号边界，并编译 C11、C++20、pkg-config 和下游
`find_package(MiniTun)` 示例。1.x 只允许向后兼容地新增接口；删除、重命名、改变结构
现有字段含义或调用约定留到 2.0。
