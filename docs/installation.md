# 安装指南

本页说明如何获取、校验并安装 MiniTun。项目聚焦资源占用最小：只有五个
可执行程序（`minitun`、`minitund`、`minitun-server`、`minitun-p2p`）与两个 SDK
共享库，无 Web GUI、无脚本运行时。正式支持 Linux/systemd。

## 发布矩阵

| 格式 | 运行环境 | 架构 |
| --- | --- | --- |
| DEB | Debian/Ubuntu | `amd64`、`arm64`、`armhf`、`riscv64` |
| RPM | Fedora/RHEL 系 | `x86_64`、`aarch64`、`armv7hl`、`riscv64` |
| OCI | Docker / containerd | `linux/amd64`、`linux/arm64`、`linux/arm/v7`、`linux/riscv64` |

每个发行版包含四个包：

| 包 | 内容 |
| --- | --- |
| `minitun-server` | 公网服务端、systemd 单元、`minitun-server` 服务账户 |
| `minitun-client` | CLI `minitun`、守护进程 `minitund`、P2P connector `minitun-p2p`、systemd 单元、`minitun` 服务账户 |
| `libminitun-client1` | 本地控制 SDK 与 Remote Protocol SDK 运行时库 |
| `libminitun-client-dev` / `libminitun-client-devel` | 开发头文件、CMake target 与 pkg-config 元数据（可选） |

## 1. 下载并校验

从 [GitHub Releases](https://github.com/Albert-Li-Sz/MiniTUN/releases) 下载目标版本与
架构的四个软件包、`SHA256SUMS` 与对应 `.sigstore.json` bundle。先校验摘要：

```bash
cd /path/to/downloads
sha256sum --check SHA256SUMS
```

再用 `cosign` 校验 keyless 签名（安装方式见 [sigstore/cosign](https://docs.sigstore.dev/cosign/installation/)）：

```bash
cosign verify-blob \
  --bundle=minitun-server_<版本>_amd64.deb.sigstore.json \
  --certificate-identity="https://github.com/Albert-Li-Sz/MiniTUN/.github/workflows/release.yml@refs/tags/<版本tag>" \
  --certificate-oidc-issuer=https://token.actions.githubusercontent.com \
  minitun-server_<版本>_amd64.deb
```

发布页同时提供 SPDX/CycloneDX SBOM 与 provenance attestation。校验失败说明传输损坏
或被篡改，不要继续安装。

## 2. DEB（Debian/Ubuntu）

```bash
sudo apt install ./minitun-server_<版本>_amd64.deb
sudo apt install ./minitun-client_<版本>_amd64.deb
# 开发 SDK 可选
sudo apt install ./libminitun-client1_<版本>_amd64.deb \
  ./libminitun-client-dev_<版本>_amd64.deb
```

安装过程会：

- 通过 systemd-sysusers 创建 `minitun-server` 与 `minitun` 专用服务账户；
- 安装并 `daemon-reload` `minitund.service` 与 `minitun-server.service`；
- **不会**生成任何凭据，**不会**自动启动服务。

验证安装：

```bash
minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
minitun-p2p --version
systemctl status minitund.service minitun-server.service
```

## 3. RPM（Fedora/RHEL）

```bash
sudo dnf install ./minitun-server-<版本>.x86_64.rpm
sudo dnf install ./minitun-client-<版本>.x86_64.rpm
# 开发 SDK 可选
sudo dnf install ./libminitun-client1-<版本>.x86_64.rpm \
  ./libminitun-client-devel-<版本>.x86_64.rpm
```

行为与 DEB 相同：创建服务账户、安装 systemd 单元、不生成凭据、不自动启动。
架构名与发行版对应：`x86_64`、`aarch64`、`armv7hl`、`riscv64`。

## 4. OCI 镜像

镜像以 `debian:stable-slim` 为基座、非 root（UID 65532）运行，不包含构建工具：

- `ghcr.io/lmtinsuzhou/minitun-server`：服务端；
- `ghcr.io/lmtinsuzhou/minitun-client`：客户端守护进程（内含 CLI 与系统 CA）。

Tag 与发布版本一致（如 `:1.0.0`）。服务端需要挂载证书、私钥、客户端
策略与 PSK 目录：

```bash
sudo mkdir -p /etc/minitun-server
# 将 server.crt、server.key、clients.json 与各客户端 PSK 放入该目录后：
docker run -d --name minitun-server \
  --network host \
  -v /etc/minitun-server:/etc/minitun-server:ro \
  ghcr.io/lmtinsuzhou/minitun-server:1.0.0
```

客户端守护进程需要持久化 `/var/lib/minitun`（状态与凭据）与 `/run/minitun`（IPC
socket）：

```bash
docker run -d --name minitund \
  --network host \
  -v /var/lib/minitun:/var/lib/minitun \
  -v /run/minitun:/run/minitun \
  ghcr.io/lmtinsuzhou/minitun-client:1.0.0
```

容器内通过 `docker exec minitund minitun daemon status` 使用同一 CLI。镜像使用
`--foreground` 直接运行二进制，退出码与日志语义与裸机部署一致。

## 5. 从源码构建

需要 CMake 3.22+、Ninja、支持 C++20 的 GCC/Clang、OpenSSL 3、SQLite3 与 Python 3。
`dev` 预设以固定版本获取其余依赖：

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

生成物位于 `build/dev/`。以 root 身份分阶段安装（只安装 Client 组件）：

```bash
cmake --install build/dev --prefix /usr --component Client
```

生产环境推荐从源码生成发行版软件包（Linux，需 `MINITUN_BUILD_PACKAGES`）：

```bash
cmake --preset package-deb   # Debian/Ubuntu
cmake --build --preset package-deb --parallel
cpack --config build/package-deb/CPackConfig.cmake -G DEB
```

详情见[开发文档](/development)。

## 6. 首次部署

### 6.1 取得客户端身份并生成 PSK

在运行 `minitund` 的内网主机上：

```bash
sudo systemctl enable --now minitund.service
minitun daemon identity --json
```

输出中的 `client_id` 是稳定身份。为它生成独立 PSK（**不得**对组或其他用户开放）：

```bash
umask 077
openssl rand -hex 32 > team-a.psk
```

### 6.2 配置公网服务端

在公网服务器上安装 `minitun-server` 后，放置证书与策略文件（全部由 `minitun-server`
服务账户拥有）：

```bash
sudo install -d -m 0750 -o minitun-server -g minitun-server \
  /etc/minitun-server/clients
sudo install -m 0600 -o minitun-server -g minitun-server team-a.psk \
  /etc/minitun-server/clients/team-a.psk
sudo install -m 0640 -o minitun-server -g minitun-server clients.json \
  /etc/minitun-server/clients.json
sudo install -m 0644 server.crt /etc/minitun-server/server.crt
sudo install -m 0600 -o minitun-server -g minitun-server server.key \
  /etc/minitun-server/server.key
```

最小策略（`clients.json`）：

```json
{
  "format_version": 1,
  "clients": [
    {
      "client_id": "client_0123456789abcdef0123456789abcdef",
      "enabled": true,
      "psk_file": "/etc/minitun-server/clients/team-a.psk",
      "allowed_ports": ["6000-6099"],
      "max_tunnels": 100,
      "max_connections": 1000,
      "max_idle_workers": 32
    }
  ]
}
```

启动并检查：

```bash
sudo systemctl enable --now minitun-server.service
systemctl status minitun-server.service
```

默认控制端口为 `2333/tcp`。云安全组与主机防火墙还必须只放行策略允许、实际使用的
公开 tunnel 端口。完整字段见[配置文档](/configuration)。

### 6.3 连接 daemon 并创建隧道

回到内网主机，把公网 `6000` 转发到本机 `127.0.0.1:8080`：

```bash
minitun server add tunnel.example.com:2333 --name edge
minitun server login edge --psk-stdin < /secure/path/team-a.psk
minitun tun add edge 8080 6000 --name web
minitun tun inspect web --json
```

当 tunnel 的 `actual_state` 为 `active` 后，公网 `tunnel.example.com:6000` 即转发到
内网 `127.0.0.1:8080`。同步是异步的；持续 `pending` 或 `failed` 时检查
`server_actual_state`、`pending_reason`、`last_error` 与双方审计日志。

UDP、SOCKS5 与 P2P mode 的创建方式：

```bash
# UDP：公网 6001/udp -> 内网 127.0.0.1:5353/udp
minitun tun add edge 5353 6001 --protocol udp --name dns-udp

# SOCKS5：公网 6002/tcp 提供 CONNECT；local-port 是兼容 CLI 的占位值
minitun tun add edge 1 6002 --protocol socks5 \
  --remote-host 127.0.0.1 --name private-proxy

# P2P：先创建入口，再在访问端运行 connector
minitun tun add edge 8080 6003 --protocol p2p --name p2p-web
minitun-p2p tunnel.example.com:6003 --listen 127.0.0.1:6501
```

SOCKS5 的 `--remote-host` 必须是数值 loopback，防止误部署为公网开放代理；P2P
connector 默认只监听 loopback。命令全集见 [CLI 文档](/cli)。

## 7. 升级与卸载

### 升级

同一发行版格式直接覆盖安装即可；普通升级与卸载保留状态目录：

- Debian：`sudo apt install ./minitun-client_<新版本>_amd64.deb ...`
- Fedora：`sudo dnf upgrade ./minitun-client-<新版本>.x86_64.rpm ...`

软件包**不会**覆盖管理员提供的证书、私钥、PSK 或客户端策略。

### 卸载

```bash
# Debian/Ubuntu：保留状态目录；purge 才会删除
sudo apt remove minitun-client minitun-server
sudo apt purge minitun-client minitun-server   # 删除 /var/lib/minitun 与 /var/lib/minitun-server

# Fedora/RHEL：卸载后状态目录保留，需手动清理
sudo dnf remove minitun-client minitun-server
```

卸载不会删除 `/etc/minitun-server` 下的证书与策略；确认不再需要时手动移除。

## 8. 排障

按顺序检查：

```bash
minitun version                     # 客户端版本与构建信息
systemctl status minitund.service minitun-server.service
journalctl -u minitund -u minitun-server -n 200 --no-pager
minitun daemon status --json        # daemon 与各 server 会话状态
minitun doctor --json --checkpoint  # SQLite 诊断、WAL checkpoint 与在线备份
```

常见问题：

- **tunnel 一直 `pending`**：确认 `server_actual_state`、`pending_reason` 与
  `last_error`；常见原因是未登录、PSK 不匹配、公网端口不在 `allowed_ports` 或策略未
  重载。
- **server 重载策略**：修改 `clients.json` 后 `sudo systemctl reload minitun-server`，
  无效新配置会保留当前快照。
- **端口被占用**：公网端口冲突映射为明确错误，释放端口后 `minitun tun enable` 即可
  恢复。
- **运维端点**：需要 `/healthz`、`/readyz`、`/metrics` 时给 daemon/server 加
  `--admin-listen 127.0.0.1:<port>`，详见[运维文档](/operations)。
