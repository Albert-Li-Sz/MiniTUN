# 变更日志

> English: [CHANGELOG.en.md](CHANGELOG.en.md)

MiniTun 的所有重要变更都会记录在此文件中。本文档以
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的结构为基础，项目
版本遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [1.1.0] - 2026-08-15

### 新增

- systemd 单元增加 `MemoryMax`/`TasksMax` 硬资源上限，用 drop-in 可按需放宽。
- 客户端策略新增 `allowed_source_cidrs` 来源白名单与 `connections_per_minute`
  每来源连接速率；`minitun-server` 新增 `--max-udp-peer-sessions`。
- 新增每日状态备份 systemd timer、OpenRC/s6 监督配方、docker-compose 示例与
  Let's Encrypt 自动续期配方。
- P2P direct path 在一次性 token 认证后升级为 TLS 1.3，以 token 作为外部 PSK
  加密应用数据，不再明文传输；relay 回退行为不变。
- tcp tunnel 支持 PROXY protocol v1 头（`tun add --proxy-protocol`）；server 在
  START_RELAY 中携带公网客户端来源端点，daemon 在本地目标前写入
  `PROXY TCP4/TCP6` 头，旧版 peer 保持字节兼容。
- `minitun-server` 管理端点新增 `/v1/*` 客户端策略管理 API：列表/查看、创建/更新、
  删除、PSK 轮换与热重载；PSK 轮换带优雅窗口（新旧 PSK 同时有效），旧会话在窗口内
  不中断，轮换响应一次性返回新 PSK。
- P2P tunnel 新增 server 辅助的 TCP simultaneous open（`tcp_simultaneous_open`
  capability）：direct 失败后双方从同一本地端口向对方的观测地址交叉 connect，
  打穿两端 EIM 映射的 NAT；`minitun-p2p` 新增 `--simultaneous-open`/
  `--no-simultaneous-open`，默认开启，对接 v1.0 daemon 需显式关闭。
- 文档站点新增英文语言（默认中文），新增 `README.en.md` 与 `CHANGELOG.en.md`。
- 发布新增 `x86_64`/`aarch64` 的 musl 完全静态二进制归档（`static.yml`，无
  glibc/OpenSSL 运行时依赖），自动附带 SHA-256。

### 移除

- 移除 `--token-stdin` 旧 CLI 别名，仅保留 `--psk-stdin`。
- 移除 schema v1–v3（v0.x 时代）迁移支持，schema v4 成为可打开的最低版本；
  更早的数据库会被拒绝且原文件保持不变。

## [1.0.0] - 2026-08-13

本代源码的首个正式版本。此前全部 v0.x 与旧版发行记录均已删除，公开历史从本版本
重新开始。

### 新增

- `tcp`、`udp`、`socks5`、`p2p` 四种 tunnel mode；非 TCP mode 通过 capability 协商，
  旧 TCP v2 wire image 保持不变。
- 新增 UDP tunnel：公网 UDP peer 使用有界 session/queue，datagram 通过认证 TLS Worker
  的 2 字节长度 record 转发到固定本地 UDP 目标，并保留报文边界。
- 新增 SOCKS5 no-auth CONNECT（IPv4、IPv6、domain）；server bind 强制为数值 loopback，
  防止误部署为公网开放代理。
- 新增 P2P mode 与 `minitun-p2p` connector：使用一次性 token 尝试 direct TCP path，
  不可达或确认失败时自动回退到原 TLS relay；支持 `--relay-only` 验证 fallback。
- 每客户端 PSK、启用状态、公开端口 ACL、tunnel/connection/idle Worker 配额，以及
  可选客户端证书 SAN/SHA-256 绑定；策略完整校验后原子热重载。
- generation-scoped `TunnelReconciler`、server/tunnel `config_revision`、注册请求
  request ID/revision 关联和最多 32 帧的有界流水线。
- 完整 server/tunnel 创建、更新、启用、禁用、logout、删除生命周期，以及严格 JSON
  `config export/plan/apply` 和安全 `--prune` ownership 语义。
- 稳定 `libminitun-client.so.1`：C11 opaque ABI、C++20 RAII `Result<T>` wrapper、
  `MiniTun::Client` CMake target、pkg-config、DEB/RPM runtime/devel 包；tunnel
  create/update 以 `struct_size` 兼容方式支持四种 mode。
- 新增 `libminitun-remote-protocol.so.1` C++20 SDK：强类型 message variant、增量 frame
  decoder、codec 和 control/Worker 认证摘要 helper，并提供 CMake/pkg-config 集成。
- daemon/server 增加默认关闭的 `/healthz`、`/readyz`、`/metrics` 管理端点；非 loopback
  强制 Bearer token，指标使用有界标签。
- 增加策略、认证、注册/注销、ACL/quota 和本地管理审计日志，不记录秘密或用户流量。
- 增加 schema 迁移、崩溃暂存凭据清理、故障注入、ABI baseline、下游 SDK、coverage、
  clang-tidy、CodeQL、持久 fuzz corpus 和独立性能/浸泡验证。
- 发布流程增加 SPDX/CycloneDX SBOM、SHA-256、GitHub OIDC provenance/attestation，
  以及可执行产物和 OCI 的 Sigstore keyless 签名验证。

### 数据与协议

- 状态库升级到 schema v5，历史 schema v4 数据自动迁移，保留稳定 ID、名称、端点、
  隧道与原凭据引用，并持久化四种 mode 与 server bind host。
- Remote Protocol v2 新增 `udp_datagrams`、`socks5_proxy`、`p2p_rendezvous` capability；
  非 TCP REGISTER/START payload 使用单字节扩展，原 TCP v2 wire image 保持不变。

### 修复与改进

- 修复隧道注册测试的顺序相关死锁：窗口请求合并为一次 TLS application write，避免
  逐帧写与提前响应互相等待。
- session 中断、generation 变化、响应乱序/重复/超时或半写入后，残留 tunnel 状态会
  确定性回到 `pending`；公开端口更新先撤销旧 listener，新绑定失败不会留下旧入口。
- TLS session resumption、Worker 自适应容量、固定缓冲 backpressure 和资源上限为正式
  100 clients / 2,000 tunnels / 10,000 relay 门禁准备。

### 安全边界

- 当前 P2P 不实现 ICE、STUN、TURN 或 NAT 打洞；direct path 在一次性 token 认证后
  升级为 TLS 1.3（token 作为外部 PSK），应用数据全程加密。

### 移除

- 移除 `minitun-gui` 本地 Web 控制台（C++ HTTP server、React/Vite 静态资源、man
  page、GUI 集成测试与全部打包引用），该组件曾随早期预发布交付。项目聚焦资源占用
  最小，控制面只有 CLI 与本地 SDK，不提供任何 Web GUI。
- 删除全部 v0.x 与旧版 v1.0.0 发行版、tag 及迁移文档；`v1.0.0` 为项目公开历史的
  唯一起点。
