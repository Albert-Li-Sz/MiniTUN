---
title: 变更日志
---

# 变更日志

完整版本记录以仓库根目录的
[CHANGELOG.md](https://github.com/LMTINSUZHOU/MiniTUN/blob/main/CHANGELOG.md) 为准。
这里保留官网常用的近期版本摘要，方便从文档站快速了解最新能力。

## [1.1.0-rc.1] - 2026-08-13

### 主要变化

- TCP 之外新增 UDP datagram、SOCKS5 no-auth CONNECT 和 P2P direct/relay fallback；
  Remote Protocol v2 通过 capability 与追加 mode byte 保持旧 TCP wire image。
- 新增 localhost-only `minitun-gui` 和独立 `minitun-p2p` connector。
- 新增 `libminitun-remote-protocol.so.1` C++20 codec/decoder/helper SDK；本地控制 SDK
  以 `struct_size` 兼容方式支持创建/更新四种 mode。
- 状态库升级到 schema v5，并补齐多传输、GUI security、Remote SDK ABI/下游链接和
  安装包布局测试。

::: warning P2P 边界
当前 P2P 不实现 ICE/STUN/TURN/NAT 打洞，direct path 不额外加密应用数据；候选不可达时
自动回退到认证 TLS relay。`v1.0.0` GA tag 保持不变，以上能力不在 1.0.0 包中。
:::

## [1.0.0] - 2026-08-11

### 主要变化

- Remote Protocol v2-only：能力协商、认证绑定 server/client/version/capabilities、
  revision-aware 有界流水线和 generation-scoped 状态收敛。
- 每客户端 PSK、可选证书绑定、端口 ACL、配额、原子 SIGHUP 重载和审计。
- schema v4、完整 server/tunnel 生命周期、声明式 config plan/apply 和安全 prune。
- `libminitun-client.so.1` 的 C11/C++20 本地控制 SDK 及 DEB/RPM runtime/devel 包。
- client/server 管理端点、Prometheus 指标、故障注入、ABI/coverage/fuzz/security 门禁，
  可选的性能/浸泡证据，以及 SBOM、keyless 签名和 provenance 发布链。

::: tip 已发布 GA
`v1.0.0` 已于 2026-08-11 发布，并与最终 `v1.0.0-rc.4` 指向同一 commit。发布时无
未解决 P0/P1，构建、打包、CodeQL/依赖安全、签名和 provenance 门禁均已通过。OCI
High/Critical 漏洞会完整报告但不阻断发布；三轮性能、24 小时压力和 7 天浸泡是可选
验证，不是 GA 前置条件。
:::

## [0.4.1] - 2026-08-08

### 安全修复

- 远程认证仅在 HMAC 验证成功后写入 nonce 重放缓存，防止未认证连接耗尽缓存并阻断
  合法客户端登录。
- 重放缓存按客户端身份隔离 nonce，并增加对应回归测试。

## [0.4.0] - 2026-08-06

### 新增

- 发布独立 Client/Server DEB 与 RPM，覆盖 Debian/Ubuntu、Fedora/RHEL 系常见架构。
- 发布最小 OCI server/client 镜像，覆盖 <code>linux/amd64</code>、<code>linux/arm64</code>、<code>linux/arm/v7</code>
  与 <code>linux/riscv64</code>。
- 增加软件包与 OCI 的安装、冒烟、跨架构验证流程。

### 移除

- 移除早期单一二进制交付路径，生产部署改为清晰的 client/server 分包。

## [0.3.0] - 2026-08-05

### 新增

- 增加多服务端连接能力，一个客户端守护进程可同时维护多个公网服务器会话。
- 增加更完整的隧道状态同步、Worker Pool 隔离与诊断输出。

### 改进

- 完善 TLS 认证、重放防护、重连退避、会话代次隔离和优雅退出。
- 强化 CI、Sanitizer、fuzz、软件包构建与发布验证。

## [0.2.x] - 2026-08-02 至 2026-08-04

- 建立远程协议、IPC 控制面、SQLite 持久化与基础 CLI。
- 增加本地开发演示、测试矩阵、排障文档和安装布局验证。
