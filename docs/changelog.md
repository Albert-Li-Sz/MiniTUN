---
title: 变更日志
---

# 变更日志

完整版本记录以仓库根目录的
[CHANGELOG.md](https://github.com/LMTINSUZHOU/MiniTUN/blob/main/CHANGELOG.md) 为准。
这里保留官网常用的近期版本摘要，方便从文档站快速了解最新能力。

## [Unreleased]

### 移除

- 移除 `minitun-gui` 本地 Web 控制台及其全部代码、静态资源与打包引用；从 1.1.0 起
  项目聚焦资源占用最小，控制面只有 CLI 与本地 SDK。
- 删除全部 v0.x 发行版与 tag；v1.0.0 与 v1.1.0 为项目公开历史的起点。

## [1.1.0-rc.1] - 2026-08-13

### 主要变化

- TCP 之外新增 UDP datagram、SOCKS5 no-auth CONNECT 和 P2P direct/relay fallback；
  Remote Protocol v2 通过 capability 与追加 mode byte 保持旧 TCP wire image。
- 新增独立 `minitun-p2p` connector。
- 新增 `libminitun-remote-protocol.so.1` C++20 codec/decoder/helper SDK；本地控制 SDK
  以 `struct_size` 兼容方式支持创建/更新四种 mode。
- 状态库升级到 schema v5，并补齐多传输、Remote SDK ABI/下游链接和安装包布局测试。

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
