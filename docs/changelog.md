---
title: 变更日志
---

# 变更日志

完整版本记录以仓库根目录的
[CHANGELOG.md](https://github.com/LMTINSUZHOU/MiniTUN/blob/main/CHANGELOG.md) 为准。
这里保留官网常用的近期版本摘要，方便从文档站快速了解最新能力。

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
