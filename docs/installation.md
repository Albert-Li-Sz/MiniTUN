# 安装指南

本文档说明如何在 Linux 主机上通过 CMake 直接安装 MiniTun。DEB 和 RPM 软件包的
构建与生命周期说明见[打包指南](packaging.md)。

## 安装组件

MiniTun 提供三个 CMake 安装组件：

| 组件 | 内容 |
| --- | --- |
| `Client` | `minitun`、`minitund`、客户端 systemd/sysusers 文件和客户端 man 手册 |
| `Server` | `minitun-server`、服务端 systemd/sysusers 文件和服务端 man 手册 |
| `Development` | 用于源码级集成与审查的公共 C++ 头文件 |

## 构建依赖

请安装 CMake 3.22 或更高版本、Ninja、支持 C++20 的编译器，以及 OpenSSL 3、
SQLite3、CLI11、独立 Asio、nlohmann/json、spdlog 和 GoogleTest 的开发软件包。
完整集成测试还需要 Python 3 和 OpenSSL 命令行工具；README 快速开始使用 curl。

Debian 或 Ubuntu 上的典型软件包名称如下：

```bash
sudo apt-get install \
  build-essential cmake ninja-build python3 openssl curl \
  libssl-dev libsqlite3-dev libcli11-dev libasio-dev \
  nlohmann-json3-dev libspdlog-dev libgtest-dev
```

Fedora 上的典型软件包名称如下：

```bash
sudo dnf install \
  gcc-c++ cmake ninja-build python3 openssl curl openssl-devel sqlite-devel \
  CLI11-devel asio-devel json-devel spdlog-devel gtest-devel
```

## 构建与验证

将安装前缀配置为 `/usr`，使生成路径与 Linux 软件包布局一致：

```bash
cmake -S . -B build/install -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_SYSCONFDIR=/etc \
  -DMINITUN_USE_SYSTEM_DEPS=ON \
  -DMINITUN_BUILD_TESTS=ON
cmake --build build/install --parallel
ctest --test-dir build/install --output-on-failure
```

接触主机文件系统前，先检查无需特权的暂存安装：

```bash
DESTDIR="$PWD/build/stage" cmake --install build/install --component Client
DESTDIR="$PWD/build/stage" cmake --install build/install --component Server
find build/stage/usr -type f -o -type l
```

安装全部运行时组件并创建服务账户：

```bash
sudo cmake --install build/install --component Client
sudo cmake --install build/install --component Server
sudo systemd-sysusers /usr/lib/sysusers.d/minitun.conf
sudo systemd-sysusers /usr/lib/sysusers.d/minitun-server.conf
sudo systemctl daemon-reload
```

程序不会自行创建 `/run/minitun`、`/var/lib/minitun`、`/run/minitun-server`
或 `/var/lib/minitun-server`。systemd 会根据 `RuntimeDirectory` 和
`StateDirectory` 指令以 `0750` 模式创建这些目录。

## 配置公网服务端

MiniTun 从不随包分发或覆盖 TLS 材料与 Token。启用服务前，请安装由管理员控制的
文件：

```bash
sudo install -d -m 0750 -o root -g minitun-server /etc/minitun-server
sudo install -m 0644 server.crt /etc/minitun-server/server.crt
sudo install -m 0600 -o minitun-server -g minitun-server \
  server.key /etc/minitun-server/server.key
sudo install -m 0600 -o minitun-server -g minitun-server \
  token /etc/minitun-server/token
```

默认服务监听 `0.0.0.0:2333`，允许公网隧道端口 `6000-6999`。如需修改配置，
请使用 systemd override，不要直接编辑发行方 unit：

```bash
sudo systemctl edit minitun-server
```

替换 `ExecStart` 时必须先清空原值：

```ini
[Service]
ExecStart=
ExecStart=/usr/bin/minitun-server --foreground \
  --listen 0.0.0.0:4433 \
  --allow-ports 10000-10999 \
  --tls-cert /etc/minitun-server/server.crt \
  --tls-key /etc/minitun-server/server.key \
  --token-file /etc/minitun-server/token
```

## 启动服务

```bash
sudo systemctl enable --now minitun-server.service
sudo systemctl enable --now minitund.service
```

客户端套接字模式为 `0660`，所有者为 `minitun:minitun`。将获授权的操作者加入
该组，然后重新登录以刷新组成员身份：

```bash
sudo usermod -aG minitun "$USER"
```

验证安装：

```bash
minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
systemctl status minitund.service minitun-server.service
journalctl -u minitund.service -u minitun-server.service
```

## 已安装布局

```text
/usr/bin/minitun
/usr/libexec/minitun/minitund
/usr/lib/systemd/system/minitund.service
/usr/lib/sysusers.d/minitun.conf
/usr/share/man/man1/minitun.1
/usr/share/man/man8/minitund.8

/usr/bin/minitun-server
/usr/lib/systemd/system/minitun-server.service
/usr/lib/sysusers.d/minitun-server.conf
/usr/share/man/man8/minitun-server.8
/etc/minitun-server/README
```

普通软件包卸载和升级会保留状态与凭据。CMake 直接安装器会将已安装文件记录在
`build/install/install_manifest.txt`；`/var/lib` 下的状态从不属于该清单。

## 升级与回滚注意事项

升级前应备份 `/var/lib/minitun`、`/var/lib/minitun-server` 以及管理员配置的 TLS
材料。MiniTun 会拒绝比当前二进制支持版本更新或发生漂移的数据库模式，不会自动
破坏或重建用户数据。回滚二进制前，请确认旧版本支持当前数据库模式。

常见启动问题及诊断方法见[故障排查](troubleshooting.md)。
