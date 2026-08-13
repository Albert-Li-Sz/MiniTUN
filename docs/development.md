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
- Node.js 22.12+ 与 npm（仅修改或验证文档站时）；
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
build/dev/minitun-p2p
build/dev/libminitun-client.so.1  # Linux；macOS 为对应 dylib
build/dev/libminitun-remote-protocol.so.1
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
| `MINITUN_ENABLE_COVERAGE` | 生成核心代码 line/branch coverage 数据 |
| `MINITUN_ENABLE_FAULT_INJECTION` | 启用测试专用 crash failpoint |
| `MINITUN_WARNINGS_AS_ERRORS` | 项目代码警告视为错误 |
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
openssl rand -hex 32 >"$MINITUN_DEMO_DIR/client.psk"
chmod 0600 "$MINITUN_DEMO_DIR/server.key" "$MINITUN_DEMO_DIR/client.psk"
```

### 2. 启动客户端守护进程

在第一个终端运行：

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
build/dev/minitund \
  --foreground \
  --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  --database "$MINITUN_DEMO_DIR/state.db" \
  --credentials "$MINITUN_DEMO_DIR/credentials.db" \
  --tls-ca "$MINITUN_DEMO_DIR/server.crt"
```

### 3. 创建客户端策略并启动服务端

在第二个终端运行：

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
bash tests/integration/write_client_policy.sh \
  build/dev/minitun "$MINITUN_DEMO_DIR/minitun.sock" \
  "$MINITUN_DEMO_DIR/clients.json" "$MINITUN_DEMO_DIR/client.psk"

build/dev/minitun-server \
  --foreground \
  --listen 127.0.0.1:2333 \
  --tls-cert "$MINITUN_DEMO_DIR/server.crt" \
  --tls-key "$MINITUN_DEMO_DIR/server.key" \
  --clients-config "$MINITUN_DEMO_DIR/clients.json"
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
  server login demo --psk-stdin <"$MINITUN_DEMO_DIR/client.psk"

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
DESTDIR="$PWD/build/stage" \
  cmake --install build/release --prefix /usr --component ClientLibrary
DESTDIR="$PWD/build/stage" \
  cmake --install build/release --prefix /usr --component ClientDevelopment
find build/stage/usr -type f -o -type l
```

开发主机需要直接安装时，可执行：

```bash
sudo cmake --install build/release --prefix /usr --component Client
sudo cmake --install build/release --prefix /usr --component Server
sudo cmake --install build/release --prefix /usr --component ClientLibrary
sudo cmake --install build/release --prefix /usr --component ClientDevelopment
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

生成 `minitun-client`、`minitun-server`、`libminitun-client1` 和
`libminitun-client-dev`/`libminitun-client-devel`。Client 包含 CLI、daemon 与 P2P
connector；两个 SDK 的 runtime 和 development 文件共用对应 library/devel 包。在 Docker
可用的 Linux 开发主机上，可以继续执行干净容器冒烟测试：

```bash
packaging/tests/smoke-deb.sh "$PWD/build/package-deb"
packaging/tests/smoke-rpm.sh "$PWD/build/package-rpm"
```

普通升级和卸载保留状态目录；Debian purge 删除状态目录，RPM 卸载则始终保留状态。
软件包不会携带或覆盖管理员提供的证书、私钥、PSK 或客户端策略。

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

| 工作流 | 内容 |
| --- | --- |
| `ci.yml` | Linux GCC/Clang、macOS 编译、完整 CTest、SDK 示例 |
| `sanitizers.yml` | ASan、UBSan、TSan、PR fuzz smoke 与 nightly corpus fuzz |
| `quality.yml` | 文档可复现构建、clang-tidy、line ≥85% / branch ≥75% coverage、ABI/downstream checks |
| `codeql.yml` | GitHub CodeQL C/C++ 扫描 |
| `reliability.yml` | tunnel registration 与高延迟 reconciliation 重复 100 次 |
| `performance.yml` | 可选的独立 4 vCPU/8 GiB 三轮基准、持久 systemd soak 与 OIDC 证据 |
| `package.yml` | 四架构 DEB/RPM、QEMU 安装测试、多架构 OCI 与非阻断漏洞报告 |
| `release.yml` | RC 连续性/冻结提交/P0-P1 门禁、SBOM、签名、attestation 与 GitHub Release |
| `pages.yml` | VitePress 文档构建与发布 |

`main` 分支包使用 `MAJOR.MINOR.PATCH_pre<运行号>~<提交号>`；它们只用于持续验收。
发布 tag 必须是 `vMAJOR.MINOR.PATCH` 或 `vMAJOR.MINOR.PATCH-rc.NUMBER`，基础版本必须与
`CMakeLists.txt` 一致。

以下是已经完成的 v1.0 发布顺序；`v1.0.0` tag 不得移动或重建：

1. 发布 `v1.0.0-rc.1` 并冻结协议、schema、SDK ABI 和功能；
2. 只修阻断项，发布 `v1.0.0-rc.2`；后续修改必须按顺序增加 `rc.N`；
3. 确认最终 RC 的必需构建、测试、打包和阻断性安全检查满足 GA 要求；
4. 确认没有未关闭的 P0/P1 issue；
5. 在最终 RC 的同一 commit 创建 `v1.0.0`。

候选版示例：

```bash
git tag -s v1.0.0-rc.1 -m "MiniTun v1.0.0-rc.1"
git push origin v1.0.0-rc.1
```

三轮性能、24 小时压力和 7 天浸泡可通过 `performance.yml` 手动执行，生成带 OIDC
attestation 的工程验证记录；`release.yml` 不下载或要求这些记录，它们的缺失或失败不会
阻止 RC 或 GA。具体启动/收集命令见[性能文档](performance.md)。

最终 RC 后任何源码变化仍必须发布后续 `rc.N`，因为 GA 必须与最终 RC 指向完全相同的
commit；这一冻结规则与可选性能/浸泡验证无关。

后续版本必须从当前源码另行创建连续 `v<版本>-rc.N`，验证最终 RC 后再在同一 commit 创建
GA tag；不得把新增能力回填到已经发布的 tag。

OCI 漏洞扫描在 RC 和 GA 中都会完整报告 High/Critical 发现，但不阻断发布。报告仍保留在
Actions 日志中供发布决策和后续基础镜像修复使用；CodeQL、依赖审计及其他安全门禁保持
阻断性。

每个架构产生 client、server、SDK runtime 和 SDK development 四个包，因此完整矩阵为
16 个 DEB 和 16 个 RPM，另有多架构 OCI。Release 还包含 SPDX/CycloneDX SBOM、
`SHA256SUMS`、每个 blob 的 Sigstore bundle 和 GitHub OIDC provenance/attestation。

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
| 服务端无法启动 | TLS 证书/私钥、`clients.json` owner/mode、各 PSK 是否为服务账户所有且模式 `0600` |
| 认证失败 | server SAN/CA、client policy ID、两端 PSK、可选证书绑定、系统时间和控制端口防火墙 |
| 隧道长期为 `pending` | `server_actual_state`、`pending_reason`、`config_revision`、`last_synced_at`、控制端口、TLS 与 PSK |
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

请勿在 Issue、日志或测试数据中提交 PSK、私钥、`credentials.db` 或未脱敏的生产
数据。
