# 本地控制 SDK

v1 提供 `libminitun-client.so.1`，只控制本机 `minitund`。它不嵌入 daemon/server，
也不暴露 JSON、Asio、SQLite、内部记录或 Remote Protocol。

## 安装与链接

Debian/Ubuntu 安装 `libminitun-client1` 和 `libminitun-client-dev`；RPM 安装
`libminitun-client1` 和 `libminitun-client-devel`。

CMake：

```cmake
find_package(MiniTun 1 REQUIRED COMPONENTS Client)
target_link_libraries(my_tool PRIVATE MiniTun::Client)
```

pkg-config：

```bash
cc -std=c11 tool.c $(pkg-config --cflags --libs minitun-client)
c++ -std=c++20 tool.cpp $(pkg-config --cflags --libs minitun-client)
```

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

## ABI 承诺

SOVERSION 为 1，默认隐藏符号，只导出 `minitun_*` 稳定 API。CI 将动态符号与
`abi/minitun-client-1.symbols` 基线比较，并编译 C11、C++20 和下游
`find_package(MiniTun)` 示例。1.x 只允许向后兼容地新增接口；删除、重命名、改变结构
现有字段含义或调用约定留到 2.0。

