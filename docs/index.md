---
layout: home

hero:
  name: MiniTun
  text: 资源占用最小的内网穿透工具
  tagline: TCP、UDP、SOCKS5、P2P 与两个稳定 SDK，共用安全的 Remote Protocol v2 控制面；无 Web GUI，无额外运行时依赖。
  image:
    src: /logo.svg
    alt: MiniTun
  actions:
    - theme: brand
      text: 安装指南
      link: /installation
    - theme: alt
      text: 查看 CLI
      link: /cli

features:
  - title: 安全传输
    details: TLS 1.2+ 与 Protocol v2；每客户端 PSK、可选证书绑定、端口 ACL、配额、重放防护与认证限速。
  - title: 持久控制面
    details: schema v5 保存稳定身份、transport mode、配置 revision 和 ownership；generation-scoped reconciler 在断线与乱序响应后确定性收敛。
  - title: 多服务器会话
    details: 一个客户端守护进程可同时连接多个公网服务器，会话、隧道与 Worker Pool 相互隔离。
  - title: 四种数据面
    details: TCP、UDP datagram、SOCKS5 CONNECT，以及具备自动 TLS relay 回退的 P2P direct path。
  - title: 最小资源占用
    details: 无 Web GUI、无脚本运行时；C++20 单一 daemon 进程 + 嵌入式 SQLite，systemd 单元自带沙箱加固，适合路由器、NAS 与边缘设备。
  - title: SDK
    details: 本地控制 C11 ABI/C++20 RAII SDK，以及独立 Remote Protocol v2 C++ codec/decoder SDK。
  - title: 生产运维
    details: client/server 都可启用 health、readiness 与 Prometheus 指标，并提供审计、热加载、诊断和成对备份。
  - title: 多架构交付
    details: 提供 Client/Server/SDK DEB、RPM 与多架构 OCI，以及 SBOM、校验和、keyless 签名和 provenance。
  - title: 持续验证
    details: CI 覆盖 GCC、Clang、Sanitizer、coverage、clang-tidy、CodeQL、fuzz、ABI、故障注入、软件包与独立性能验证。
---

## 工作原理

MiniTun 由公网服务端 <code>minitun-server</code>、客户端守护进程 <code>minitund</code>、
命令行工具 <code>minitun</code>、P2P connector <code>minitun-p2p</code> 和两个 SDK
组成。CLI 和本地 SDK 只通过受权限保护的 Unix IPC 管理 daemon；远程控制和 Worker 使用
TLS 下的 Protocol v2。

<div class="minitun-flow">
  <div class="flow-node">公网 TCP / UDP / SOCKS5 / P2P 客户端</div>
  <div class="flow-link">→</div>
  <div class="flow-node">minitun-server<br>公网端口</div>
  <div class="flow-link">⇄ TLS Worker</div>
  <div class="flow-node">minitund<br>内网服务</div>
</div>

## 生产部署

部署前请准备一台公网服务器、一台可以访问目标服务的内网主机、服务端域名与有效 TLS
证书，并确定实际需要对外开放的 TCP/UDP 端口。

| 软件包 | 运行环境 | 发布架构 |
| --- | --- | --- |
| DEB | Debian/Ubuntu | <code>amd64</code>、<code>arm64</code>、<code>armhf</code>、<code>riscv64</code> |
| RPM | Fedora/RHEL 系 | <code>x86_64</code>、<code>aarch64</code>、<code>armv7hl</code>、<code>riscv64</code> |
| OCI | Docker / containerd | <code>linux/amd64</code>、<code>linux/arm64</code>、<code>linux/arm/v7</code>、<code>linux/riscv64</code> |

::: tip 推荐路径
生产环境优先使用 Release 中的 DEB/RPM 或 OCI 镜像；如果目标版本尚未发布，请按
[开发文档](/development) 在受信任的构建环境中生成并验证软件包。
:::

## 常用命令

<div class="mini-command">

~~~bash
minitun daemon status
minitun daemon identity --json
minitun health
minitun readiness
minitun metrics
minitun doctor --json --checkpoint
minitun server list
minitun server inspect primary --json
minitun tun list primary
minitun tun inspect web --json
minitun-p2p tunnel.example.com:6003 --listen 127.0.0.1:6501
~~~

</div>

完整命令、JSON 输出与退出码见 [CLI 文档](/cli)。

## 文档入口

- [安装指南](/installation)：校验签名、DEB/RPM/OCI 安装、源码构建、首次部署与卸载。
- [命令行界面](/cli)：生命周期命令、PSK 输入、JSON 输出与退出码。
- [配置与策略](/configuration)：每客户端策略和声明式资源。
- [系统架构](/architecture)：schema v5、reconciler、session、Worker 与多种数据面。
- [Remote Protocol v2](/protocol)：能力协商、认证、注册与数据中继。
- [SDK](/sdk)：本地控制 C11/C++20 API 与 Remote Protocol C++20 codec/decoder。
- [运维与可观测性](/operations)：管理端点、指标、审计和备份。
- [v1 迁移](/migration-v1)：从 v0.4.1 协调升级与离线回滚。
- [性能与浸泡验证](/performance)：可选三轮基准、24 小时压力与 7 天浸泡。
- [开发文档](/development)：源码构建、本地演示、测试、打包、发布与排障。
- [变更日志](/changelog)：近期版本变更记录。
