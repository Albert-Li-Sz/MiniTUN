---
title: 变更日志
---

# 变更日志

完整版本记录以仓库根目录的
[CHANGELOG.md](https://github.com/LMTINSUZHOU/MiniTUN/blob/main/CHANGELOG.md) 为准。
这里保留官网常用的近期版本摘要，方便从文档站快速了解最新能力。

## [Unreleased] - v1.0.0

### 主要变化

- Remote Protocol v2-only：能力协商、认证绑定 server/client/version/capabilities、
  revision-aware 有界流水线和 generation-scoped 状态收敛。
- 每客户端 PSK、可选证书绑定、端口 ACL、配额、原子 SIGHUP 重载和审计。
- schema v4、完整 server/tunnel 生命周期、声明式 config plan/apply 和安全 prune。
- `libminitun-client.so.1` 的 C11/C++20 本地控制 SDK 及 DEB/RPM runtime/devel 包。
- client/server 管理端点、Prometheus 指标、故障注入、ABI/coverage/fuzz/security 门禁，
  可选的性能/浸泡证据，以及 SBOM、keyless 签名和 provenance 发布链。

::: warning 尚未发布 GA
`v1.0.0` tag 要求连续的 rc.1、rc.2（及后续 RC）、无未解决 P0/P1，并与最终 RC 指向
同一 commit；构建、打包、安全扫描、签名和 provenance 门禁仍须通过。三轮性能、24 小时
压力和 7 天浸泡不是 GA 前置条件。
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
