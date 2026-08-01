# MiniTun

[![CI](https://github.com/LMTINSUZHOU/MiniTUN/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/LMTINSUZHOU/MiniTUN/actions/workflows/ci.yml)
[![Sanitizers](https://github.com/LMTINSUZHOU/MiniTUN/actions/workflows/sanitizers.yml/badge.svg?branch=main)](https://github.com/LMTINSUZHOU/MiniTUN/actions/workflows/sanitizers.yml)
[![Packages](https://github.com/LMTINSUZHOU/MiniTUN/actions/workflows/package.yml/badge.svg?branch=main)](https://github.com/LMTINSUZHOU/MiniTUN/actions/workflows/package.yml)

MiniTun 是一个面向 Linux、使用 C++20 独立实现的轻量级 TCP 反向隧道系统。它由
本地命令行客户端、客户端守护进程和公网服务端组成，通过经过认证的 TLS 控制会话与
预连接 Worker 将公网 TCP 端口安全地转发到客户端所在网络中的本地 TCP 服务。

MiniTun 不使用也不兼容 FRP 线协议。本项目聚焦边界明确、资源有界、可测试且适合
systemd 部署的 TCP 反向隧道能力。

## 项目状态

| 项目 | 状态 |
| --- | --- |
| 当前版本 | `0.1.0` |
| 开发阶段 | 阶段 0 至阶段 16 已完成 |
| 最终验收 | 已通过，详见[最终验收记录](docs/acceptance.md) |
| 主要平台 | Linux x86_64；源码也可在受支持依赖齐备的平台构建 |
| 语言与标准 | C++20、CMake 3.22+、Ninja |
| 远程传输 | TLS 1.2 或更高版本上的自定义有界二进制协议 |
| 软件包 | 独立的 Client/Server DEB 与 RPM |
| 许可证 | [MIT](LICENSE) |

当前 `main` 分支已经通过多编译器、ASan、UBSan、TSan、libFuzzer、多服务器 E2E、
DEB/RPM 干净容器安装和 systemd unit 校验。创建生产发布前仍应由维护者选择提交并
显式推送版本 tag。

## 核心特性

- **多服务器隔离**：`minitund` 为每个公网服务端维护独立 TLS 控制会话、心跳、
  重连控制器、会话代次、隧道集合和 Worker Pool；单个服务端故障不会中断其他会话。
- **安全的本地控制面**：无状态 `minitun` CLI 只通过受保护的 Unix 套接字访问
  `minitund`，不直接打开 SQLite 或连接公网服务端。
- **可靠持久化**：服务器、隧道和稳定客户端身份保存在经过版本迁移与完整性校验的
  SQLite 数据库中；重启后自动归一化和恢复状态。
- **独立凭据边界**：Token 单独保存在权限精确为 `0600` 的凭据数据库中，不进入
  `state.db`、命令参数、IPC 响应或常规日志。
- **TLS 认证控制协议**：支持证书与主机名校验、HMAC-SHA256 质询认证、nonce 重放
  保护、时钟偏差检查、认证限速、心跳和 `GOAWAY`。
- **有界 Worker Pool**：按会话与全局限制空闲 Worker、等待时间和连接总数，并在
  会话代次变化时回收陈旧连接。
- **有背压的 TCP 中继**：双向各使用固定 16 KiB 缓冲区，写完再读，支持 TCP
  半关闭、空闲超时、取消和流量统计，不存在无界数据队列。
- **故障恢复与优雅关闭**：支持指数退避重连、服务端/守护进程重启恢复、信号处理和
  有期限的活动中继排空。
- **Linux 原生交付**：提供 CMake 组件安装、systemd unit、systemd-sysusers、
  man 手册，以及经过容器测试的 DEB/RPM 软件包。
- **持续验证**：GitHub Actions 覆盖 GCC/Clang、完整 CTest、Sanitizer、fuzz、
  打包、安装冒烟测试和 tag 发布流程。

## 架构概览

```text
操作者 -> minitun CLI -> Unix IPC -> minitund -> 本地 TCP 服务
                                      ^    |
                                      |    | TLS 控制会话与 Worker
                                      |    v
公网 TCP 客户端 ----------------> minitun-server
```

控制面只同步身份、隧道 ID 和公网绑定信息；本地目标主机与端口不会发送给公网服务端。
完整组件关系、状态恢复和资源所有权见[系统架构](docs/architecture.md)，线协议见
[远程协议](docs/protocol.md)。

## 快速开始

以下流程用于本机功能体验，不替代生产部署。需要 Ninja、CMake 3.22+、支持 C++20
的编译器、OpenSSL 3、SQLite3、Python 3 和 curl。不同 Linux 发行版的完整依赖
列表见[安装指南](docs/installation.md)。

### 1. 构建并运行测试

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

### 2. 创建本地演示凭据

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
install -d -m 0700 "$MINITUN_DEMO_DIR"

openssl req -x509 -newkey rsa:3072 -nodes \
  -keyout "$MINITUN_DEMO_DIR/server.key" \
  -out "$MINITUN_DEMO_DIR/server.crt" \
  -days 1 \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'
openssl rand -hex 32 >"$MINITUN_DEMO_DIR/token"
chmod 0600 "$MINITUN_DEMO_DIR/server.key" "$MINITUN_DEMO_DIR/token"
```

### 3. 启动公网服务端

在第一个终端中设置同一个 `MINITUN_DEMO_DIR`，然后运行：

```bash
build/dev/minitun-server \
  --foreground \
  --listen 127.0.0.1:2333 \
  --tls-cert "$MINITUN_DEMO_DIR/server.crt" \
  --tls-key "$MINITUN_DEMO_DIR/server.key" \
  --token-file "$MINITUN_DEMO_DIR/token" \
  --allow-ports 6000-6999
```

### 4. 启动客户端守护进程

在第二个终端中运行：

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
build/dev/minitund \
  --foreground \
  --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  --database "$MINITUN_DEMO_DIR/state.db" \
  --credentials "$MINITUN_DEMO_DIR/credentials.db" \
  --tls-ca "$MINITUN_DEMO_DIR/server.crt"
```

### 5. 启动本地目标并注册隧道

在第三个终端中启动一个本地测试服务：

```bash
python3 -m http.server 8080 --bind 127.0.0.1
```

在第四个终端中配置服务器和隧道：

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  server add localhost:2333 --name demo

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  server login demo --token-stdin <"$MINITUN_DEMO_DIR/token"

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  tun add demo 8080 6000 --name demo-http
```

状态同步是异步的。重复执行以下命令，直至 `actual_state` 变为 `active`：

```bash
build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  tun inspect demo-http --json
```

随后通过公网监听端口访问本地服务：

```bash
curl http://127.0.0.1:6000/
```

演示结束后请停止三个前台进程，并清理 `build/demo-runtime` 中的一次性凭据与状态。
再次运行时应使用空目录。CLI 完整语义和退出码见 [CLI 参考](docs/cli.md)。

## 生产安装

生产环境应优先使用经过 CI 验证的 DEB/RPM 产物，或按[安装指南](docs/installation.md)
执行 CMake 组件安装。安装后需要由管理员提供 CA、服务端证书、私钥和 Token，并在
启动服务前审查监听地址、`--allow-ports`、连接上限和 systemd override。

默认生产路径：

```text
/run/minitun/minitun.sock
/var/lib/minitun/state.db
/var/lib/minitun/credentials.db
/etc/minitun-server/server.crt
/etc/minitun-server/server.key
/etc/minitun-server/token
```

MiniTun 不会自动生成或分发生产凭据，也不会自动启用服务。

## 文档

| 文档 | 内容 |
| --- | --- |
| [文档索引](docs/README.md) | 全部用户、运维、开发和发布文档导航 |
| [安装指南](docs/installation.md) | 依赖、CMake 安装、systemd 配置和升级注意事项 |
| [CLI 参考](docs/cli.md) | 命令、Token 输入、隧道语义和退出码 |
| [系统架构](docs/architecture.md) | 组件、持久化、会话隔离、Worker 与中继生命周期 |
| [远程协议](docs/protocol.md) | 帧格式、认证、隧道注册、Worker 和原始中继 |
| [安全设计](docs/security.md) | 已实现控制、威胁边界和资源约束 |
| [开发指南](docs/development.md) | 构建预设、测试、Sanitizer、fuzz 和开发原则 |
| [打包指南](docs/packaging.md) | DEB/RPM 构建、生命周期和发布产物要求 |
| [CI 与发布](docs/ci.md) | 工作流、产物命名和 tag 发布过程 |
| [故障排查](docs/troubleshooting.md) | 服务、TLS、IPC、隧道和软件包常见问题 |
| [最终验收](docs/acceptance.md) | 阶段 16 证据、正式产物校验和发布结论 |

## 项目边界

MiniTun 当前仅提供 TCP 反向隧道，不实现 UDP、P2P/NAT 穿透、SOCKS5、流量压缩
或单连接多路复用。每条活动中继使用独立 TLS Worker。项目也不承诺兼容 FRP 的
配置、API 或线协议。

## 安全

部署前请阅读[安全策略](SECURITY.md)和[安全设计](docs/security.md)。不要在公开
issue、日志或测试数据中提交真实 Token、私钥或证书。疑似漏洞应按安全策略通过
GitHub Security Advisory 私下报告。

## 参与贡献与获取支持

欢迎提交缺陷修复、测试、文档和经过讨论的功能改进。开始前请阅读
[贡献指南](CONTRIBUTING.md)与[社区行为准则](CODE_OF_CONDUCT.md)。使用问题、缺陷
报告所需信息和支持边界见[支持说明](SUPPORT.md)。

## 许可证

MiniTun 采用 [MIT License](LICENSE)。
