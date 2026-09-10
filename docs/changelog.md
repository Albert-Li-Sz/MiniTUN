---
title: 变更日志
---

# 变更日志

完整版本记录以仓库根目录的
[CHANGELOG.md](https://github.com/Albert-Li-Sz/MiniTUN/blob/main/CHANGELOG.md) 为准。
这里保留官网常用的近期版本摘要，方便从文档站快速了解最新能力。

## [1.2.3] - 2026-09-11

- 服务端强制 SOCKS5 注册使用数值 loopback，直接发送注册的客户端也不能绕过校验。
- P2P direct context 复用统一显式 TLS 策略，继续仅使用 TLS 1.3 和一次性 token external PSK。
- UDP record 在读取 payload 前拒绝超过 65,507 字节的声明，并增加超长 wire record 回归。
- 当前文档同步 schema v6、direct TLS 加密、TCP simultaneous open 与新增 capability 说明。

## [1.2.2] - 2026-09-09

- 服务端启动时比较 `--max-total-connections` 与可见内存天花板，默认值放不下时输出警告，
  不再让 OOM kill 来暴露这个冲突。
- TLS 套件策略显式固定：TLS 1.2 仅 ECDHE+AEAD，TLS 1.3 仅 AEAD 套件。
- 需要认证的管理端点拒绝不指向监听地址的 `Host`（`421 Misdirected Request`），阻断
  DNS rebinding。
- 认证重放缓存改为 O(1) 过期，不再在每次认证时全量扫描。
- Remote Protocol SDK 的 ABI 门禁由符号数量改为符号基线比对。
- 静态 musl 构建升级到 OpenSSL 3.5 LTS；仓库移除损坏的 `sqlite.zip`。

## [1.2.1] - 2026-09-05

- TLS listener 在分配 TLS 对象前限制接入速率、全局及每 IP 未认证连接数；耗尽预算时
  定时等待，同一来源反复握手失败后临时拒绝。
- TLS 与应用认证共用总超时期限，认证或清理完成后释放未认证配额。
- 重复 TLS 错误日志每 5 秒最多一条，新增待认证连接、TLS 失败与接入拒绝指标。
- 增加真实 TLS/明文洪泛、慢连接、配额释放、心跳和关闭回归测试。
- 修复 Linux NAT 穿透测试的公网网桥拓扑，并在失败时输出网络与进程诊断。
- 修复 P2P 打洞端口复用，并限制快速失败时的重试频率，避免 CPU 与 SYN 洪泛。

## [1.2.0] - 2026-08-18

- P2P NAT 打洞补齐 `worker_observed_endpoint` capability，并新增 netns/iptables 双 EIM
  NAT e2e 测试验证 TCP simultaneous open 的 direct 穿透。
- daemon 指标新增 UDP-over-P2P 的 datagram/字节计数（`minitun_p2p_udp_*_total`）。
- 新增 admin HTTP 解析 fuzz target 与持久语料。
- quality 门禁新增中英文文档同步校验，防止双语漂移。

## [1.1.1] - 2026-08-15

- P2P 路径新增 UDP 转发（`minitun-p2p --udp`），direct 与 relay 路径均支持。

## [1.1.0] - 2026-08-15

- 状态库升级至 schema v6，新增默认关闭的 `tunnels.proxy_protocol`；v4/v5 数据自动在
  事务中迁移至 v6，保留现有资源、配置与凭据引用。回滚需恢复升级前的成对备份。

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
当前 P2P 支持 server 辅助的 TCP simultaneous open，不实现 ICE/STUN/TURN 或 UDP 打洞；
direct path 经 TLS 1.3 加密，以一次性 token 作为外部 PSK；直连失败时自动回退到认证
TLS relay。上述 TLS 升级与 TCP 打洞自 v1.1.0 引入，v1.2.0 补齐 NAT 候选观测地址。
:::

::: tip 已发布
`v1.0.0` 已于 2026-08-13 发布，是本代源码的首个正式版本。此前全部 v0.x 与旧版发行
记录已删除，公开历史从本版本重新开始。安装方式见[安装指南](/installation)。
:::
