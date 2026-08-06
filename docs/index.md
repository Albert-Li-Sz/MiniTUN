---
layout: home

hero:
  name: MiniTun
  text: 轻量、安全的 TCP 反向隧道
  tagline: 将公网服务器上的 TCP 端口稳定转发到内网 Linux 主机，适合以 systemd 服务长期运行。
  image:
    src: /logo.svg
    alt: MiniTun
  actions:
    - theme: brand
      text: 快速部署
      link: /#生产部署
    - theme: alt
      text: 查看 CLI
      link: /cli

features:
  - title: 安全传输
    details: 远程连接使用 TLS 1.2+，并通过 HMAC-SHA256 质询完成 Token 认证，包含重放防护、时钟偏差检查与认证限速。
  - title: 持久控制面
    details: 服务器、隧道和客户端身份保存在 SQLite 中；服务重启后自动恢复连接与隧道状态。
  - title: 多服务器会话
    details: 一个客户端守护进程可同时连接多个公网服务器，会话、隧道与 Worker Pool 相互隔离。
  - title: 生产运维
    details: 内置 health、readiness、metrics、reload 与 doctor 命令，支持诊断、热加载与 SQLite 在线备份。
  - title: 多架构交付
    details: 提供 Client/Server DEB、RPM 与 OCI 镜像，覆盖 amd64、arm64、armhf、riscv64 等常见 Linux 架构。
  - title: 持续验证
    details: CI 覆盖 GCC、Clang、CTest、Sanitizer、libFuzzer、软件包安装测试与 OCI 镜像发布验证。
---

## 工作原理

MiniTun 由公网服务端 <code>minitun-server</code>、客户端守护进程 <code>minitund</code> 和命令行工具 <code>minitun</code>
组成。公网服务端负责接受客户端会话并监听对外 TCP 端口；客户端守护进程保存配置、维护
会话与 Worker，并连接内网本地服务；CLI 通过受权限保护的 Unix 套接字管理本地守护进程。

<div class="minitun-flow">
  <div class="flow-node">公网 TCP 客户端</div>
  <div class="flow-link">→</div>
  <div class="flow-node">minitun-server<br>公网端口</div>
  <div class="flow-link">⇄ TLS Worker</div>
  <div class="flow-node">minitund<br>内网服务</div>
</div>

## 生产部署

部署前请准备一台公网服务器、一台可以访问目标服务的内网主机、服务端域名与有效 TLS
证书，并确定实际需要对外开放的 TCP 端口。

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
minitun health
minitun readiness
minitun metrics
minitun doctor --json --checkpoint
minitun server list
minitun server inspect primary --json
minitun tun list primary
minitun tun inspect web --json
~~~

</div>

完整命令、JSON 输出与退出码见 [CLI 文档](/cli)。

## 文档入口

- [命令行界面](/cli)：命令、Token 输入、隧道语义与退出码。
- [系统架构](/architecture)：组件、持久化、会话、Worker 与中继生命周期。
- [远程协议](/protocol)：帧格式、认证、隧道注册与数据中继。
- [开发文档](/development)：源码构建、本地演示、测试、打包、发布与排障。
- [变更日志](/changelog)：近期版本变更记录。
