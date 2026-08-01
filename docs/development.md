# 开发指南

本文档面向希望构建、测试或修改 MiniTun 的开发者。提交变更前，请同时阅读仓库根目录
的[贡献指南](../CONTRIBUTING.md)。

## 前置条件

- CMake 3.22 或更高版本
- Ninja
- 支持 C++20 的编译器
- OpenSSL 3 开发文件
- SQLite3 开发文件

`dev` 预设通过 FetchContent 使用锁定版本的 CLI11、独立 Asio、nlohmann/json、
spdlog 和 GoogleTest；`release` 预设使用系统软件包。

## 配置、构建与测试

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

常用配置开关包括：

```text
MINITUN_USE_SYSTEM_DEPS
MINITUN_BUILD_TESTS
MINITUN_BUILD_FUZZERS
MINITUN_ENABLE_ASAN
MINITUN_ENABLE_UBSAN
MINITUN_ENABLE_TSAN
MINITUN_ENABLE_LTO
MINITUN_BUILD_PACKAGES
MINITUN_PACKAGE_VERSION
```

Sanitizer 构建使用独立预设。TSan 有意与 ASan/UBSan 分开：

```bash
cmake --preset asan
cmake --build --preset asan --parallel 2
ctest --preset asan

cmake --preset ubsan
cmake --build --preset ubsan --parallel 2
ctest --preset ubsan

cmake --preset tsan
cmake --build --preset tsan --parallel 2
ctest --preset tsan
```

使用 Clang 和 libFuzzer 构建并冒烟测试全部 fuzz 目标：

```bash
cmake --preset fuzz
cmake --build --preset fuzz --parallel 2
for target in remote_frame ipc_frame ipc_json endpoint port_range; do
  "build/fuzz/minitun_${target}_fuzz" -runs=2000 -max_total_time=10
done
```

Apple Command Line Tools 自带的 Clang 可能不包含 libFuzzer 运行时。使用 Homebrew
LLVM 时，可在首次配置 fuzz 预设时指定：

```text
-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
```

## 本地控制面与多服务器开发

IPC 线格式由四字节网络字节序负载长度和随后的 UTF-8 JSON 组成，双向均限制为
1 MiB。公共协议层会在请求到达方法处理器前执行严格模式校验；Unix 域套接字层在
不向 CLI 暴露 SQLite 的情况下处理并发单请求会话。处理器执行限制在四个工作线程
中，并始终受每会话绝对期限约束。守护进程控制服务提供独立凭据存储、全部资源命令、
JSON 输出、稳定退出码和受保护的 Token 输入。

在私有目录中执行真实的守护进程状态往返：

```bash
runtime_root="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
runtime_dir="$(mktemp -d "$runtime_root/minitun.XXXXXX")"
build/dev/minitund \
  --socket "$runtime_dir/minitun.sock" \
  --database "$runtime_dir/state.db" \
  --credentials "$runtime_dir/credentials.db" \
  --tls-ca /path/to/server-ca.crt \
  --io-threads 4
# 在另一个终端中运行：
build/dev/minitun --socket "$runtime_dir/minitun.sock" daemon status
build/dev/minitun --socket "$runtime_dir/minitun.sock" \
  server add example.com:2333 --name primary
build/dev/minitun --socket "$runtime_dir/minitun.sock" \
  tun add primary 22 6000 --name ssh
```

只运行 IPC 相关测试：

```bash
ctest --test-dir build/dev --output-on-failure \
  -R '(Ipc|Frame|Dispatcher|Credential|DaemonControl|cli-daemon)'
```

IPC 测试使用相互隔离且已解析为物理路径的临时套接字路径。解码器测试覆盖分片帧和
合并帧；传输测试覆盖畸形客户端隔离、期限、并发请求、单请求连接、线程池关闭、
权限、可信路径祖先、串行化启动和清理。CLI/守护进程集成测试还会验证 CRUD、JSON、
重启恢复、PTY 无回显输入、Token 泄漏扫描、墓碑过滤以及文档列出的全部退出码类别。

`integration.multi-server-sessions` 会生成临时 CA/证书，启动两个相互独立的公网
服务端，验证同时在线状态和故障隔离，然后重启其中一个服务端及守护进程，以证明
系统能够自动恢复。

## 持久化开发

两种依赖模式下，存储实现都使用系统 SQLite3 库。默认生产路径为
`/var/lib/minitun/state.db`，但单元测试会创建隔离的临时数据库文件，不会访问
该生产路径。

修改持久化代码后，只运行存储与恢复测试：

```bash
ctest --test-dir build/dev --output-on-failure -R '(Storage|Recovery|Credential)'
```

测试覆盖首次与重复迁移、拒绝未来版本/模式漂移/畸形模式、迁移回滚、连接策略、
事务提交/回滚/隔离、仓库校验与约束、单调时间戳、墓碑行为、重启状态恢复、凭据
权限与 CRUD、不安全链接与父目录，以及并发守护进程修改。

`minitund` 接受 `--database` 和 `--credentials`，打开两个存储、执行状态恢复、
检查凭据引用，然后才启动 IPC。测试必须传入私有临时目录内的路径；程序不会创建
生产环境父目录。

## 变更验证原则

开发者应根据改动范围选择最小但充分的测试集，并在提交前至少完成一次常规构建与
完整 CTest。涉及并发、生命周期、解析器、安全边界或打包的变更，还应分别运行相关
Sanitizer、fuzz 或容器软件包测试。完整验收矩阵见[最终验收记录](acceptance.md)。
