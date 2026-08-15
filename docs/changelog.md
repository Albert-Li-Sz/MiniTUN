---
title: 变更日志
---

# 变更日志

完整版本记录以仓库根目录的
[CHANGELOG.md](https://github.com/Albert-Li-Sz/MiniTUN/blob/main/CHANGELOG.md) 为准。
这里保留官网常用的近期版本摘要，方便从文档站快速了解最新能力。

## [1.1.0] - 2026-08-15

- tcp tunnel 支持 PROXY protocol v1 头（`--proxy-protocol`），旧版 peer 保持字节兼容。
- `minitun-server` 新增 `/v1/*` 客户端策略管理 API（列表/创建/更新/删除/PSK 轮换），
  轮换带优雅窗口，旧会话不中断。
- 客户端策略新增来源 CIDR 白名单与每来源连接速率；systemd 增加内存/任务上限。
- 发布新增 musl 完全静态二进制归档（`static.yml`）。
- P2P 新增 TCP simultaneous open（server 辅助 NAT 打洞），`--simultaneous-open`
  默认开启，失败自动回退 relay。
- 文档站新增英文语言（默认中文）。

## [1.0.0] - 2026-08-13

### 主要变化

- TCP、UDP datagram、SOCKS5 no-auth CONNECT 与 P2P direct/relay fallback 四种 tunnel
  mode；Remote Protocol v2 通过 capability 与追加 mode byte 保持旧 TCP wire image。
- 独立 `minitun-p2p` connector；每客户端 PSK、端口 ACL、配额与审计。
- `libminitun-remote-protocol.so.1` C++20 codec/decoder/helper SDK；本地控制 C11/C++20
  SDK 以 `struct_size` 兼容方式支持创建/更新四种 mode。
- 状态库 schema v5，自动迁移历史 v3/v4 数据；无 Web GUI、无脚本运行时，聚焦最小资源
  占用，适合路由器、NAS 与边缘设备。

::: warning P2P 边界
当前 P2P 不实现 ICE/STUN/TURN/NAT 打洞；direct path 经 TLS 1.3 PSK 加密；候选不可达时
自动回退到认证 TLS relay。
:::

::: tip 已发布
`v1.0.0` 已于 2026-08-13 发布，是本代源码的首个正式版本。此前全部 v0.x 与旧版发行
记录已删除，公开历史从本版本重新开始。安装方式见[安装指南](/installation)。
:::
