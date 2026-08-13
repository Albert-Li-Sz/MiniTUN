# 变更日志

MiniTun 的所有重要变更都会记录在此文件中。本文档以
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的结构为基础，项目
版本遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### 移除

- 移除 `minitun-gui` 本地 Web 控制台（C++ HTTP server、React/Vite 静态资源、man
  page、GUI 集成测试与全部打包引用），该组件曾随 `v1.1.0-rc.1` 预发布交付。从 1.1.0
  起项目聚焦资源占用最小，控制面只有 CLI 与本地 SDK，不提供任何 Web GUI。
- 删除全部 v0.x 发行版与 tag；v1.0.0 与 v1.1.0 为项目公开历史的起点。

## [1.1.0-rc.1] - 2026-08-13

### 新增

- 新增 `udp` tunnel：公网 UDP peer 使用有界 session/queue，datagram 通过认证 TLS Worker
  的 2 字节长度 record 转发到固定本地 UDP 目标，并保留报文边界。
- 新增 SOCKS5 no-auth CONNECT（IPv4、IPv6、domain）；server bind 强制为数值 loopback，
  防止误部署为公网开放代理。
- 新增 P2P mode 与 `minitun-p2p` connector：使用一次性 token 尝试 direct TCP path，
  不可达或确认失败时自动回退到原 TLS relay；支持 `--relay-only` 验证 fallback。
- 新增 `libminitun-remote-protocol.so.1` C++20 SDK：强类型 message variant、增量 frame
  decoder、codec 和 control/Worker 认证摘要 helper，并提供 CMake/pkg-config 集成。
- 本地 C/C++ 控制 SDK 的 tunnel create/update 以 `struct_size` 兼容方式新增 `protocol`
  与 `remote_host` 字段；旧 1.0 调用方继续按 TCP 默认值运行。

### 数据与协议

- 状态库升级到 schema v5，事务迁移原 tunnel 为 `tcp`/`0.0.0.0`，并持久化四种 mode 与
  server bind host。
- Remote Protocol v2 新增 `udp_datagrams`、`socks5_proxy`、`p2p_rendezvous` capability；
  非 TCP REGISTER/START payload 使用单字节扩展，原 TCP v2 wire image 保持不变。

### 交付与验证

- Client 软件包新增 `minitun-p2p` 与 man pages；SDK runtime/development 包同时交付
  Remote Protocol 共享库、头文件、CMake target 和 pkg-config 元数据。
- 新增 UDP/SOCKS5/P2P 端到端、P2P direct/fallback、Remote SDK ABI/下游链接、
  schema v5 迁移和安装布局回归测试。

### 安全边界

- 当前 P2P 不实现 ICE、STUN、TURN 或 NAT 打洞；一次性 token 只认证 direct candidate，
  direct application data 不额外加密，敏感应用必须自行使用 TLS。
- `v1.0.0` GA tag 保持不可变；以上能力只属于 1.1.0 开发线。

## [1.0.0] - 2026-08-11

### 不兼容变更

- Remote Protocol 升级为 v2-only，HELLO/ACK 显式协商能力；v0.4.x 客户端与服务端
  不能混用，升级要求协调停机。
- 公网 server 用严格 `--clients-config` JSON 取代单一 `--token-file`。
- 状态数据库迁移到 schema v4；v0.4.1 不能打开迁移后数据库，回滚必须恢复升级前备份。

### 新增

- 增加每客户端 PSK、启用状态、公开端口 ACL、tunnel/connection/idle Worker 配额，
  以及可选客户端证书 SAN/SHA-256 绑定；策略完整校验后原子热重载。
- 增加 generation-scoped `TunnelReconciler`、server/tunnel `config_revision`、注册请求
  request ID/revision 关联和最多 32 帧的有界流水线。
- 增加 `daemon.identity`，完整 server/tunnel update/enable/disable/logout 生命周期，
  以及严格 JSON `config export/plan/apply` 和安全 `--prune` ownership 语义。
- 增加稳定 `libminitun-client.so.1`：C11 opaque ABI、C++20 RAII `Result<T>` wrapper、
  `MiniTun::Client` CMake target、pkg-config、DEB/RPM runtime/devel 包。
- daemon/server 增加默认关闭的 `/healthz`、`/readyz`、`/metrics` 管理端点；非 loopback
  强制 Bearer token，指标使用有界标签。
- 增加策略、认证、注册/注销、ACL/quota 和本地管理审计日志，不记录秘密或用户流量。
- 增加 schema v3→v4 迁移、崩溃暂存凭据清理、故障注入、ABI baseline、下游 SDK、
  coverage、clang-tidy、CodeQL、持久 fuzz corpus 和独立性能/浸泡验证。
- 发布流程增加 SPDX/CycloneDX SBOM、SHA-256、GitHub OIDC provenance/attestation，
  以及可执行产物和 OCI 的 Sigstore keyless 签名验证；性能/浸泡工作流可另行生成绑定
  commit 的 OIDC 证据，但不是发布门禁。

### 修复与改进

- 修复隧道注册测试的顺序相关死锁：窗口请求现在合并为一次 TLS application write，
  避免逐帧写与提前响应互相等待；重复回归不再依赖工作目录、执行顺序或端口复用时机。
- session 中断、generation 变化、响应乱序/重复/超时或半写入后，残留 tunnel 状态会
  确定性回到 `pending`。公开端口更新先撤销旧 listener，新绑定失败不会留下旧入口。
- TLS session resumption、Worker 自适应容量、固定缓冲 backpressure 和资源上限为正式
  100 clients / 2,000 tunnels / 10,000 relay 门禁准备。

### 发布状态

- `v1.0.0` GA 已于 2026-08-11 发布；tag 与最终 annotated `v1.0.0-rc.4` 指向同一
  commit，且发布时无未解决 P0/P1。构建、打包、CodeQL/依赖安全、签名和 provenance
  门禁均已通过；OCI High/Critical 漏洞保留完整报告但不阻断发布，独立三轮基准、
  24 小时压力和 7 天浸泡为可选验证。
