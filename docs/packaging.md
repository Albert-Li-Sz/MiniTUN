# 打包指南

MiniTun 使用 CPack 生成两个组件软件包：

| 软件包 | CMake 组件 | 内容 |
| --- | --- | --- |
| `minitun-client` | `Client` | `minitun`、`minitund`、客户端 unit、sysusers 定义和 man 手册 |
| `minitun-server` | `Server` | `minitun-server`、服务端 unit、sysusers 定义、man 手册和配置说明 |

生产软件包布局使用 `/usr` 存放已安装程序与共享数据、`/etc` 存放管理员配置、
`/usr/lib/systemd/system` 存放 unit、`/usr/lib/sysusers.d` 存放账户定义。
两个运行时软件包都不包含公共开发头文件。

## 构建 DEB 软件包

在 Ubuntu 22.04 或更高版本上，安装[安装指南](installation.md)列出的构建依赖，
再安装 `dpkg-dev`、`fakeroot` 和 `file`，然后运行：

```bash
cmake --preset package-deb
cmake --build --preset package-deb --parallel
ctest --test-dir build/package-deb --output-on-failure
cpack --config build/package-deb/CPackConfig.cmake -G DEB
packaging/tests/verify-deb.sh build/package-deb
```

生成文件使用 Debian 原生命名：

```text
minitun-client_0.1.0_amd64.deb
minitun-server_0.1.0_amd64.deb
```

发布自动化可以设置 `MINITUN_PACKAGE_VERSION`。稳定版本使用
`MAJOR.MINOR.PATCH`；候选发布使用 `MAJOR.MINOR.PATCH-rc.NUMBER`，并转换为
符合 Debian 版本排序规则的 `MAJOR.MINOR.PATCH~rc.NUMBER` 元数据。基础版本必须
与 CMake 项目版本一致。

直接检查软件包：

```bash
dpkg-deb -I build/package-deb/minitun-client_0.1.0_amd64.deb
dpkg-deb -c build/package-deb/minitun-client_0.1.0_amd64.deb
```

## 构建 RPM 软件包

在 Fedora 上，安装[安装指南](installation.md)列出的依赖和 `rpm-build`，然后运行：

```bash
cmake --preset package-rpm
cmake --build --preset package-rpm --parallel
ctest --test-dir build/package-rpm --output-on-failure
cpack --config build/package-rpm/CPackConfig.cmake -G RPM
packaging/tests/verify-rpm.sh build/package-rpm
```

生成文件使用 RPM 原生命名：

```text
minitun-client-0.1.0-1.x86_64.rpm
minitun-server-0.1.0-1.x86_64.rpm
```

候选发布的 RPM 元数据使用稳定基础版本和类似 `0.rc.1` 的 release 值，以保持
正确的 RPM 升级顺序。

直接检查软件包：

```bash
rpm -qip build/package-rpm/minitun-client-0.1.0-1.x86_64.rpm
rpm -qlp build/package-rpm/minitun-client-0.1.0-1.x86_64.rpm
```

## 生命周期行为

两种软件包格式都会在安装时运行 `systemd-sysusers`，并在 systemd 可用时请求
`systemctl daemon-reload`。软件包不会自动启用或启动服务，因此可以安全地安装在
容器中，管理员也能在首次启动前完成凭据配置。

MiniTun 软件包绝不包含 `server.crt`、`server.key` 或 `token`，也不会覆盖管理员
提供的 TLS 或 Token 文件。重复安装、升级和普通卸载都会保留
`/var/lib/minitun` 与 `/var/lib/minitun-server`。Debian purge 会删除这些状态
目录；RPM 没有独立 purge 操作，因此 RPM 卸载会保留状态。

## 容器冒烟测试

将生成的软件包复制或挂载到 `/packages` 后，在干净的 amd64/x86_64 容器中运行
可复用冒烟测试：

```bash
packaging/tests/smoke-deb.sh /packages
packaging/tests/smoke-rpm.sh /packages
```

测试会安装两个软件包，验证三个版本命令，确认 unit 与 sysusers 文件，模拟升级，
卸载软件包，并在不启动 systemd 的情况下验证文档约定的状态保留策略。

## 发布产物要求

发布产物必须来自通过完整测试套件和干净容器冒烟测试的同一次工作流运行，并附带
SHA-256 清单。不得使用未经工作流验证的本地软件包替换发布产物。自动化流程与命名
约定见 [CI 与发布自动化](ci.md)，已验收产物见[最终验收记录](acceptance.md)。
