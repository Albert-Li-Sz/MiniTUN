# 开发文档

本文档汇总 MiniTun 的源码构建、本地运行、非软件包安装、测试、打包、发布和开发排障。
生产环境请使用[项目首页](/)中的软件包与 systemd 部署流程。

## 开发环境

基础要求：

- Linux 或 macOS；完整的 systemd 与软件包测试需要 Linux；
- CMake 3.22 或更高版本；
- Ninja；
- 支持 C++20 的 GCC 或 Clang；
- OpenSSL 3、SQLite3 和 Python 3；
- 可访问 FetchContent 上游依赖的网络环境。

Debian/Ubuntu 的最小开发依赖：

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends --yes \
  build-essential cmake curl git libsqlite3-dev libssl-dev ninja-build \
  openssl pkg-config python3
```

Fedora 的最小开发依赖：

```bash
sudo dnf install \
  cmake curl gcc-c++ git ninja-build openssl openssl-devel \
  pkgconf-pkg-config python3 sqlite-devel
```

`dev` 预设会以校验过的固定版本获取 CLI11、独立 Asio、nlohmann/json、spdlog 和
GoogleTest。`release` 预设使用发行版提供的系统依赖；使用它时还需安装对应的
CLI11、Asio、nlohmann/json、spdlog 与 GoogleTest 开发包。

## 构建与测试

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

生成的程序位于 `build/dev/`：

```text
build/dev/minitun
build/dev/minitund
build/dev/minitun-server
```

常用 CMake 选项：

| 选项 | 作用 |
| --- | --- |
| `MINITUN_USE_SYSTEM_DEPS` | 使用系统依赖而不是固定版本的 FetchContent 依赖 |
| `MINITUN_BUILD_TESTS` | 构建 CTest 测试 |
| `MINITUN_BUILD_FUZZERS` | 构建 libFuzzer 目标 |
| `MINITUN_ENABLE_ASAN` | 启用 AddressSanitizer |
| `MINITUN_ENABLE_UBSAN` | 启用 UndefinedBehaviorSanitizer |
| `MINITUN_ENABLE_TSAN` | 启用 ThreadSanitizer |
| `MINITUN_ENABLE_LTO` | 启用链接时优化 |
| `MINITUN_BUILD_PACKAGES` | 启用 CPack 软件包生成 |
| `MINITUN_PACKAGE_VERSION` | 设置稳定版或候选版软件包版本 |

只运行一组测试时可使用 CTest 正则表达式：

```bash
ctest --test-dir build/dev --output-on-failure -R '(Storage|Recovery|Credential)'
ctest --test-dir build/dev --output-on-failure -R '(Ipc|Dispatcher|DaemonControl)'
ctest --test-dir build/dev --output-on-failure -R '(MultiServer|Tunnel|Worker|Relay)'
```

## 本地完整演示

以下流程全部运行在本机，仅用于开发和功能验证。

### 1. 创建临时凭据

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
install -d -m 0700 "$MINITUN_DEMO_DIR"

openssl req -x509 -newkey rsa:3072 -nodes \
  -keyout "$MINITUN_DEMO_DIR/server.key" \
  -out "$MINITUN_DEMO_DIR/server.crt" \
  -days 1 \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'
openssl rand -hex 32 >"$MINITUN_DEMO_DIR/token"
chmod 0600 "$MINITUN_DEMO_DIR/server.key" "$MINITUN_DEMO_DIR/token"
```

### 2. 启动服务端

在第一个终端运行：

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
build/dev/minitun-server \
  --foreground \
  --listen 127.0.0.1:2333 \
  --tls-cert "$MINITUN_DEMO_DIR/server.crt" \
  --tls-key "$MINITUN_DEMO_DIR/server.key" \
  --token-file "$MINITUN_DEMO_DIR/token"
```

### 3. 启动客户端守护进程

在第二个终端运行：

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
build/dev/minitund \
  --foreground \
  --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  --database "$MINITUN_DEMO_DIR/state.db" \
  --credentials "$MINITUN_DEMO_DIR/credentials.db" \
  --tls-ca "$MINITUN_DEMO_DIR/server.crt"
```

### 4. 启动目标服务

在第三个终端运行：

```bash
python3 -m http.server 8080 --bind 127.0.0.1
```

### 5. 注册并验证隧道

在第四个终端运行：

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  server add localhost:2333 --name demo

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  server login demo --token-stdin <"$MINITUN_DEMO_DIR/token"

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  tun add demo 8080 6000 --name demo-http

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  tun inspect demo-http --json

curl http://127.0.0.1:6000/
```

隧道同步是异步的；如果状态仍为 `pending`，稍后重新执行 `tun inspect`。完成后停止
三个前台进程，并删除 `build/demo-runtime` 中的一次性凭据与状态。

## CMake 暂存与直接安装

使用系统依赖完成 Release 构建：

```bash
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
```

在接触系统目录前，先执行无特权暂存安装并检查文件布局：

```bash
DESTDIR="$PWD/build/stage" \
  cmake --install build/release --prefix /usr --component Client
DESTDIR="$PWD/build/stage" \
  cmake --install build/release --prefix /usr --component Server
find build/stage/usr -type f -o -type l
```

开发主机需要直接安装时，可执行：

```bash
sudo cmake --install build/release --prefix /usr --component Client
sudo cmake --install build/release --prefix /usr --component Server
sudo systemd-sysusers /usr/lib/sysusers.d/minitun.conf
sudo systemd-sysusers /usr/lib/sysusers.d/minitun-server.conf
sudo systemctl daemon-reload
```

systemd 会根据 unit 中的 `RuntimeDirectory` 和 `StateDirectory` 创建运行与状态目录。
直接安装不会自动生成 TLS 材料，也不会自动启用服务。生产主机优先使用 README 中的
DEB/RPM 流程，以便由软件包管理器跟踪文件与生命周期。

## Sanitizer 与 fuzz

ASan 与 UBSan 的组合构建：

```bash
cmake --preset asan
cmake --build --preset asan --parallel 2
ctest --preset asan
```

独立 UBSan 与 TSan 构建：

```bash
cmake --preset ubsan
cmake --build --preset ubsan --parallel 2
ctest --preset ubsan

cmake --preset tsan
cmake --build --preset tsan --parallel 2
ctest --preset tsan
```

使用 Clang 和 libFuzzer 构建全部 fuzz 目标：

```bash
cmake --preset fuzz
cmake --build --preset fuzz --parallel 2
for target in remote_frame ipc_frame ipc_json endpoint port_range; do
  "build/fuzz/minitun_${target}_fuzz" -runs=2000 -max_total_time=10
done
```

Apple Command Line Tools 的 Clang 可能不包含 libFuzzer 运行时。使用 Homebrew LLVM
时，可在首次配置时追加：

```text
-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
```

## 构建 DEB 与 RPM

DEB 构建需要 `dpkg-dev`、`fakeroot` 和 `file`：

```bash
cmake --preset package-deb
cmake --build --preset package-deb --parallel
ctest --test-dir build/package-deb --output-on-failure
cpack --config build/package-deb/CPackConfig.cmake -G DEB
packaging/tests/verify-deb.sh build/package-deb
```

RPM 构建需要 Fedora 系统依赖与 `rpm-build`：

```bash
cmake --preset package-rpm
cmake --build --preset package-rpm --parallel
ctest --test-dir build/package-rpm --output-on-failure
cpack --config build/package-rpm/CPackConfig.cmake -G RPM
packaging/tests/verify-rpm.sh build/package-rpm
```

生成的软件包分别为 `minitun-client` 和 `minitun-server`。在 Docker 可用的 Linux
开发主机上，可以继续执行干净容器冒烟测试：

```bash
packaging/tests/smoke-deb.sh "$PWD/build/package-deb"
packaging/tests/smoke-rpm.sh "$PWD/build/package-rpm"
```

普通升级和卸载保留状态目录；Debian purge 删除状态目录，RPM 卸载则始终保留状态。
软件包不会携带或覆盖管理员提供的证书、私钥和 Token。

## 交叉编译 DEB/RPM 与 OCI 镜像

### 交叉编译 DEB/RPM

`package.yml` 在 ubuntu-24.04 上用发行版交叉工具链构建
`arm64`/`armhf`/`riscv64` 的 DEB 与 `aarch64`/`armv7hl`/`riscv64` 的 RPM：
`dpkg --add-architecture`（riscv64 额外添加 ports 软件源）安装
`libssl-dev:<arch>` 与 `libsqlite3-dev:<arch>`，CMake 通过
`CMAKE_SYSTEM_PROCESSOR`、`<triplet>-g++` 与 `CPACK_*_PACKAGE_ARCHITECTURE`
生成目标架构包。交叉 DEB 使用显式 `Depends`（关闭 shlibdeps），交叉 RPM 依赖
rpmbuild 的 ELF 依赖扫描生成 soname 级 `Requires`。每个新架构都在 QEMU 容器中
安装并运行 `minitun version` / `minitund --version` / `minitun-server --version`
冒烟验证。

### OCI 镜像

`packaging/oci/Dockerfile.server` 与 `Dockerfile.client` 以 `debian:stable-slim`
为基座，直接拷贝交叉构建出的二进制（镜像内不编译），以非 root 用户
（UID 65532）运行；客户端镜像通过 `ca-certificates` 内置系统 CA。`package.yml`
的 OCI job 从 DEB 产物提取二进制、按架构构建镜像并推送到
`ghcr.io/lmtinsuzhou/minitun-server` 与 `ghcr.io/lmtinsuzhou/minitun-client`，
随后用 `docker manifest` 汇总为覆盖 amd64/arm64/arm/v7/riscv64 的多架构清单。

## CI 与发布

GitHub Actions 包含四条工作流：

| 工作流 | 内容 |
| --- | --- |
| `ci.yml` | GCC/Clang 构建、完整 CTest 与 CLI 冒烟测试 |
| `sanitizers.yml` | ASan、UBSan、TSan 与有界 fuzz 测试 |
| `package.yml` | DEB/RPM 四架构（amd64/arm64/armhf/riscv64 与 x86_64/aarch64/armv7hl/riscv64）交叉编译与 QEMU 容器安装冒烟；Release 时构建并发布 OCI 镜像 |
| `release.yml` | 校验版本 tag，复用打包工作流并创建 GitHub Release |

`main` 分支触发的包使用 `MAJOR.MINOR.PATCH_pre<运行号>~<提交号>` 开发版本；
文件名中的 `~` 会替换为 `-`。这些产物只用于持续验收，不与正式 Release 共用包版本。

发布 tag 必须是 `vMAJOR.MINOR.PATCH` 或 `vMAJOR.MINOR.PATCH-rc.NUMBER`，且基础版本
必须与 `CMakeLists.txt` 中的项目版本一致：

```bash
git tag -a v0.4.0 -m "MiniTun v0.4.0"
git push origin v0.4.0
```

发布工作流仅在全部软件包测试通过后创建 GitHub Release。每个版本包含
四个架构的 Client/Server DEB（共八个）、四个架构的 Client/Server RPM（共八个）、
一份覆盖全部产物的 `SHA256SUMS`，以及发布到 `ghcr.io` 的 OCI 多架构镜像。

## 开发排障

先检查版本、进程状态与最近日志：

```bash
minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
systemctl status minitund.service minitun-server.service
journalctl -u minitund.service -u minitun-server.service --since '-10 min'
```

常见问题：

| 现象 | 检查项 |
| --- | --- |
| CLI 退出码为 `3` | `minitund` 是否运行、套接字是否为 `0660`、当前用户是否属于 `minitun` 组 |
| 服务端无法启动 | 证书与私钥是否匹配；Token 是否为服务账户所有、模式是否为 `0600` |
| 认证失败 | 证书 SAN、CA 信任、两端 Token、系统时间和控制端口防火墙 |
| 隧道长期为 `pending` | `minitun tun inspect <name> --json` 的 `server_actual_state`、`pending_reason`、`last_synced_at` 与 `last_error`，以及控制端口连通性、TLS 和 Token |
| 隧道状态为 `failed` | `permission_denied`、`resource_exhausted` 或 `remote_port_in_use` 错误码 |
| 公网端口无法访问 | 本地目标、映射端口的云安全组与主机防火墙、隧道状态和连接配额 |

本地开发时，套接字与数据库必须位于当前用户所有、模式为 `0700` 的真实目录中。
MiniTun 会拒绝符号链接、不安全父目录、错误权限、模式漂移或未来版本数据库。不要通过
放宽文件权限或直接删除数据库绕过检查；应先备份文件，再根据日志定位原因。

`state.db` 使用 WAL 模式，运行期间的最新事务可能位于 `state.db-wal`；
`credentials.db` 使用带安全删除的 DELETE journal。检查或备份活动数据库时必须使用
SQLite 在线备份接口或同时保留主文件及 `-wal`、`-shm`，不得在 `minitund` 运行时
删除任何 sidecar 文件。`remove` 成功后的逻辑结果应通过 `minitun list/inspect` 或
SQLite 连接查询，不应只比较主文件时间戳。

请勿在 Issue、日志或测试数据中提交 Token、私钥、`credentials.db` 或未脱敏的生产
数据。
