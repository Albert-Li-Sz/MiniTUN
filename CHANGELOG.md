# 变更日志

MiniTun 的所有重要变更都会记录在此文件中。本文档以
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的结构为基础，项目
版本遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased] - v1.0.0

### 不兼容变更

- Remote Protocol 升级为 v2-only，HELLO/ACK 显式协商能力；v0.4.x 客户端与服务端
  不能混用，升级要求协调停机。
- 公网 server 用严格 `--clients-config` JSON 取代单一 `--token-file`。
- 状态数据库迁移到 schema v4；v0.4.1 不能打开迁移后数据库，回滚必须恢复升级前备份。

### 新增

- 增加每客户端 PSK、启用状态、公开端口 ACL、tunnel/connection/idle Worker 配额，
  以及可选客户端证书 SAN/SHA-256 绑定；策略完整校验后原子热重载。
- 增加 generation-scoped `TunnelReconciler`、server/tunnel `config_revision`、注册请求
  request ID/revision 关联和最多 32 帧的有界流水线。
- 增加 `daemon.identity`，完整 server/tunnel update/enable/disable/logout 生命周期，
  以及严格 JSON `config export/plan/apply` 和安全 `--prune` ownership 语义。
- 增加稳定 `libminitun-client.so.1`：C11 opaque ABI、C++20 RAII `Result<T>` wrapper、
  `MiniTun::Client` CMake target、pkg-config、DEB/RPM runtime/devel 包。
- daemon/server 增加默认关闭的 `/healthz`、`/readyz`、`/metrics` 管理端点；非 loopback
  强制 Bearer token，指标使用有界标签。
- 增加策略、认证、注册/注销、ACL/quota 和本地管理审计日志，不记录秘密或用户流量。
- 增加 schema v3→v4 迁移、崩溃暂存凭据清理、故障注入、ABI baseline、下游 SDK、
  coverage、clang-tidy、CodeQL、持久 fuzz corpus 和独立性能/浸泡门禁。
- 发布流程增加 SPDX/CycloneDX SBOM、SHA-256、GitHub OIDC provenance/attestation，
  可执行产物和 OCI 的 Sigstore keyless 签名验证，以及绑定 commit 的性能/浸泡证据门禁。

### 修复与改进

- 修复隧道注册测试的顺序相关死锁：窗口请求现在合并为一次 TLS application write，
  避免逐帧写与提前响应互相等待；重复回归不再依赖工作目录、执行顺序或端口复用时机。
- session 中断、generation 变化、响应乱序/重复/超时或半写入后，残留 tunnel 状态会
  确定性回到 `pending`。公开端口更新先撤销旧 listener，新绑定失败不会留下旧入口。
- TLS session resumption、Worker 自适应容量、固定缓冲 backpressure 和资源上限为正式
  100 clients / 2,000 tunnels / 10,000 relay 门禁准备。

### 发布状态

- 代码版本已进入 1.0.0 开发线；创建 rc.1、rc.2 或 GA tag 前仍必须归档独立三轮基准、
  24 小时满规模压力和随后 7 天混合负载浸泡证据；release workflow 会拒绝缺失、缩短、
  错序或非同一提交的证据。

## [0.4.1] - 2026-08-08

### 安全修复

- 远程认证在 HMAC 验证成功后才写入 nonce 重放缓存，避免未持有 Token 的对端通过
  伪造 `AUTH` 请求耗尽共享缓存并阻断合法客户端认证。
- 重放缓存键加入规范化 `client_id`，避免不同客户端的 nonce 发生不必要的全局冲突。

### 测试

- 增加伪造 HMAC 不消耗 nonce、不同客户端 nonce 隔离的回归测试；完整单元测试增至
  222 项。

## [0.4.0] - 2026-08-06

### 新增

- DEB 新增 `arm64`/`armhf`/`riscv64`，RPM 新增 `aarch64`/`armv7hl`/`riscv64`
  交叉编译与 QEMU 容器安装冒烟测试。
- 新增最小 OCI server/client 镜像（`debian:stable-slim` 基座、非 root 运行、
  client 内置 CA），Release 发布 `ghcr.io/lmtinsuzhou/minitun-server` 与
  `minitun-client` 的 amd64/arm64/arm/v7/riscv64 多架构清单。

### 移除

- 移除 OpenWrt 打包与发布支持（`packaging/openwrt`、签名软件源与 GitHub Pages
  发布流程），项目只交付 DEB、RPM 与 OCI 镜像。

## [0.3.0] - 2026-08-05

### 新增

- 增加 `health`、`readiness`、`metrics`、`reload` 与 `doctor` 本地运维 IPC/CLI
  方法；`doctor` 支持 SQLite 诊断、WAL checkpoint、在线备份和受控恢复。
- `minitund` 与 `minitun-server` 支持 `SIGHUP` 热加载，分别重建远程会话或重新读取
  TLS 证书、私钥和 Token，无需重启进程监听器。
- 服务端增加跨所有已认证客户端的 `--max-total-tunnels` 隧道总配额。

### 改进

- 远程会话的凭据读取、隧道协调和状态持久化使用共享的单线程 DB executor，完成后
  返回所属会话 strand，避免 SQLite 操作阻塞网络状态机。
- 隧道注册与注销使用最多 32 条请求的有界流水线窗口，先批量释放旧监听器，再批量
  注册新监听器，减少高延迟链路上的协调往返次数。
- 新增进程内运行指标快照，覆盖会话、Worker、连接、重连次数及持久化/协议错误计数，
  并累计 Worker 配额拒绝和成功中继双向字节数；字段结构保持稳定。
- `doctor restore` 在写入任一数据库前校验所有请求源的 schema、完整性和文件权限，
  拒绝无关或未来版本数据库，并明确单库原子、双库逐库提交的恢复边界。
- 服务端热加载为每条会话保留不可变 TLS 与 Token 快照；控制会话和空闲 Worker
  使用新凭据重连，活动中继继续排空，避免多 I/O 线程认证竞态和连接中断。

### 测试

- 本地完整 CTest 共 235 项通过，包含 221 项单元测试、10 项集成测试以及 TLS、TCP
  中继、多服务器、稳定性和安装布局验证；新增双库恢复预验证和热加载中继保护测试。

## [0.2.4] - 2026-08-04

### 改进

- 客户端从服务端心跳中协商 Worker 空闲超时，并保留 5 秒关闭宽限；服务端配置
  `1-300` 秒时，客户端不再按固定 65 秒提前回收 Worker。
- `tun inspect` 与隧道 JSON 新增 `server_actual_state`、`pending_reason` 和
  `last_synced_at`，可直接区分未认证、断线、退避、等待远端同步等 `pending` 原因。
- 状态数据库升级到模式版本 3，持久化隧道最近一次远端同步时间。

### 修复

- 服务器会话断开时通过单个事务批量将其活动隧道更新为 `pending`，降低 SQLite
  事务数量与 OpenWrt 闪存写放大；持久化失败不再静默丢弃，而会写入结构化日志。
- 服务器删除、凭据轮换与启动恢复共用同一套凭据清理逻辑，同时清除历史引用和双槽
  稳定键，避免迁移数据遗留孤立凭据；控制面会串行执行登录与删除的跨数据库清理，
  防止并发登录误删较新的凭据槽。
- Worker 空闲超时状态使用与其 `1-305` 秒范围匹配的 32 位原子值，避免 32 位 MIPS
  OpenWrt 工具链因不必要的 64 位原子操作引入 `libatomic` 链接依赖。

### 测试

- 增加批量 `pending` 事务回滚、持久化失败日志、Worker 超时协商边界、诊断字段与
  历史凭据清理回归测试。
- AArch64 OpenWrt 包验证增加 QEMU 完整运行测试，覆盖 TLS 认证、守护进程、SQLite
  模式升级、状态与凭据持久化、真实 TCP 隧道传输，以及守护进程重启恢复。

## [0.2.3] - 2026-08-04

### 修复

- 控制面 `server`/`tun` 变更提交后会直接唤醒对应远程会话并完成隧道协调；新增隧道
  不再等待下一次心跳，删除命令也会立即释放公网监听，心跳仅保留为状态同步兜底。
- 离线会话收到批量隧道变更时保留当前重连期限，不再为每次通知立即发起一次连接并
  将指数退避推至上限；服务端恢复后可及时上线并协调全部待处理隧道。
- 主 TLS 监听器与每个隧道监听器在 `accept` 持续失败时使用有上限的指数退避和
  限频日志；遇到 `EMFILE`/`ENFILE` 时通过预留文件描述符接收并关闭一个排队连接，
  避免描述符或内核缓冲耗尽形成 CPU 热循环。
- 服务端 I/O 工作线程异常会汇总到主线程、记录内部错误并返回非零退出码，使
  `Restart=on-failure` 能够按预期恢复进程。
- 服务端为控制连接、Worker 和公网连接分配独立 strand，仅将 Worker Pool、隧道
  注册表和待分配队列保留在轻量控制 strand 上；`--io-threads` 现在可并行处理多个
  会话和中继连接。
- 修复 GCC 11 与较早 Asio 组合下跨 strand 异步操作状态的生命周期问题，避免认证、
  隧道注册和 Worker 分配路径发生重复释放。
- `--token-stdin` 改为逐字节有界读取，最多检查 65,537 字节并在超限时立即拒绝，
  不再由 `std::getline` 为任意长度输入扩展内存。
- CMake 显式跟踪 Git `HEAD`、当前引用和 tag 元数据；提交或 tag 变化后执行普通构建
  会自动刷新二进制版本信息，并由测试核对实际 Git 提交。
- `main` 分支软件包使用包含 Actions 运行号和提交号的预发布版本，避免开发构建与
  正式 Release 共享 DEB、RPM 或 OpenWrt 包版本。
- 隧道注册集成测试使用非临时端口池，避免测试连接占用刚释放的临时端口而造成
  `EADDRINUSE` 随机失败。

### 测试

- 增加控制提交通知、离线批量变更、监听退避分类与限频、超长 Token、60 秒心跳下
  即时隧道协调、Git 提交匹配以及多连接并发中继回归验证。

## [0.2.2] - 2026-08-04

### 修复

- 调整控制会话心跳与隧道同步顺序：客户端优先回复 `PONG`，服务端在心跳间隔内持续处理注册与注销请求，避免高延迟或大量隧道场景中因同步阻塞心跳而长期处于 `pending` 状态。
- Token 登录改用双槽凭据轮换，并为被替换会话增加持久化写屏障；认证中的旧会话无法覆盖新凭据对应状态，跨 `state.db` 与 `credentials.db` 操作失败后可安全恢复。
- 将服务器与隧道删除的 SQLite 事务提交设为逻辑成功边界，并使用非阻塞 WAL checkpoint；外部读取者占用 WAL 或提交后清理延迟不再导致“数据已删除但命令报错”，残留墓碑由后台任务或启动恢复继续清理。
- 公网连接等待 Worker 改为按客户端及会话代次索引的事件驱动 FIFO 队列，移除每连接 25 毫秒轮询与 Worker 到达时的全量扫描，降低高并发等待场景的 CPU 开销与调度延迟。
- 增加高延迟多隧道协调、认证中 Token 轮换、WAL 读取者占用及凭据清理失败等回归测试。

## [0.2.1] - 2026-08-03

### 修复

- `tun remove` 与 `server remove` 现在会物理删除 SQLite 状态行，名称和公网端口可
  立即重新创建；守护进程也会清理旧版本或中断操作遗留的墓碑记录。
- 调整跨数据库删除顺序：先持久化状态删除，再清理独立凭据库，避免活动服务器记录
  引用已经删除的 Token。
- `pending` 隧道现在继承父级会话错误；未登录、TCP、TLS 与认证故障可直接从
  `last_error` 判断，不再长期显示无原因的 `null`。
- DEB/RPM systemd 服务与 OpenWrt procd 服务仅授予
  `CAP_NET_BIND_SERVICE`，使专用非 root 服务账户能够绑定 `1-65535` 中配置允许的
  端口，包括 `82` 等特权端口。
- 公网监听绑定权限错误现在稳定映射为 `permission_denied`，与端口占用错误区分。

## [0.2.0] - 2026-08-02

### 变更

- 软件包提供的 `minitun-server.service` 不再将隧道端口限制为 `6000-6999`，默认使用
  服务端的完整有效 TCP 端口范围 `1-65535`；管理员仍可通过 `--allow-ports` 设置
  白名单。

### 新增

- OpenWrt 25.12 APK v3 客户端与服务端包，包含 UCI 默认配置、
  procd 服务、专用服务账户和保守的禁用状态。
- `x86_64`、AArch64、ARMv7、MIPS 大端、MIPS 小端与 RISC-V 64 六种
  OpenWrt 发布架构。
- 基于官方 SDK SHA-256 校验、APK v3 元数据/布局检查与 QEMU 启动的
  多架构打包验收流程。
- 基于 C++20、CMake 和 Ninja 的项目基础。
- 系统依赖模式与锁定版本的 FetchContent 依赖模式。
- 通用错误、结果、结构化日志和构建版本模块。
- 严格的域名、IPv4、带方括号 IPv6 端点解析和 TCP 端口范围解析。
- 由 OpenSSL CSPRNG 支持的强类型 128 位随机 ID。
- 防溢出的 Unix/单调时间辅助函数和线程安全 UTC 格式化。
- 可移动但不可复制的秘密存储，并使用 OpenSSL 显式清除内存。
- 有界的结构化日志字段和 UTF-8 安全截断。
- 事务化 SQLite 模式版本迁移，以及 WAL、外键、同步、busy timeout、检查点和
  日志大小策略校验。
- 已校验的 `ServerRepository` 和 `TunnelRepository` CRUD、墓碑、存储限制、
  确定性查询和跨仓库事务。
- 持久化服务器和隧道状态的原子化重启归一化与恢复快照。
- 严格限制为 1 MiB、长度前缀的 UTF-8 JSON IPC 请求与响应编解码器。
- 线程安全的方法分派、稳定错误响应和异常隔离。
- 并发 Unix 域套接字客户端/服务端传输，包含期限、连接上限、精确 `0660` 权限、
  可信路径校验、串行化启动和安全的陈旧套接字清理。
- `minitun daemon status` 与本地 `minitund` 进程的真实通信。
- 完整的 `server`、`tun` 和聚合 `status` CLI 命令，支持 JSON 列表/详情输出以及
  稳定的 `0/2/3/4/5/10` 退出码。
- 由一致性 SQLite 事务、重启恢复、墓碑过滤和并发本地请求处理支撑的守护进程
  控制服务。
- 独立的 SQLite 凭据存储：文件权限为 `0600`，支持事务化 put/get/remove、
  模式检查、安全删除，并由 `state.db` 保存不透明引用。
- 无回显交互式 Token 输入、显式 `--token-stdin`，以及 Token 相关 CLI/IPC
  缓冲区清除。
- 带版本的 24 字节远程二进制帧，显式网络字节序编码、严格 64 KiB 上限、增量
  流解码和完整消息类型。
- 有界二进制负载原语、严格 UTF-8 字段、控制/Worker 连接状态校验，以及支持
  ASan/UBSan 的 libFuzzer 远程帧目标。
- TLS 1.2 或更高版本的服务端传输，包含证书/密钥校验、显式分帧、有界握手与
  心跳期限以及固定数量的 Asio I/O 线程。
- HELLO/AUTH 消息编解码器、HMAC-SHA256 质询认证、常量时间摘要校验、有界 nonce
  重放缓存、按地址失败限速、隔离的会话代次和通用认证失败响应。
- 运行时生成证书的集成测试，覆盖可信/不可信 TLS、正确/错误 Token、心跳交换和
  不泄漏 Token 的服务端日志。
- 模式版本 2 迁移，并事务化生成可跨重启保持稳定的守护进程客户端身份。
- 守护进程侧多服务器管理器，提供隔离的 TLS 控制会话、认证、心跳状态、会话代次
  和带抖动的逐服务器指数退避重连。
- `minitund` CA 选择、固定 I/O 线程数、结构化日志级别选择，以及带显著警告、
  仅供开发使用的 TLS 校验绕过选项。
- 双服务器集成测试，覆盖故障隔离、服务端重启恢复、守护进程重启恢复和稳定身份复用。
- 有界 REGISTER/UNREGISTER 隧道负载编解码器，以及相关联的成功响应和稳定失败响应。
- 服务端公网监听器注册表，包含数值地址校验、显式端口白名单、逐客户端隧道上限、
  幂等删除和端口冲突映射。
- 守护进程隧道同步，持久化 `registering`、`active`、`failed`、`pending` 和
  `removing` 状态，并在重连后恢复监听器。
- 注册集成测试，覆盖策略拒绝、端口冲突与恢复、监听器释放以及守护进程/服务端重启恢复。
- 按代次隔离的 Worker Pool，包含逐服务器与全局空闲容量上限、自动补充、公网连接
  等待期限和空闲过期。
- 双向 TCP 中继，双向各使用固定 16 KiB 缓冲区，支持读写背压、半关闭传播、
  空闲期限、取消和字节统计。
- 优雅信号处理，包含尽力而为的 `GOAWAY`、有期限的中继排空、逐客户端与全局
  连接配额，以及重启/恢复集成测试。
- 支持组件的 Linux 安装，包含加固的 systemd unit、systemd-sysusers 定义、
  man 手册、公共头文件和暂存安装布局校验。
- 独立的 `minitun-client` 与 `minitun-server` DEB/RPM 软件包，包含服务账户创建、
  daemon-reload 钩子、保留状态的升级/卸载、DEB purge 清理、软件包内容检查和
  干净容器冒烟测试。
- GitHub Actions 编译器、Sanitizer、有界 fuzz、DEB/RPM、OpenWrt 多架构和 tag 发布工作流，
  包含可复用的已测试打包流程、版本化产物、SHA-256 清单、最小 Token 权限、
  并发取消和分组 Dependabot 更新。
- 简体中文开源项目文档，包括生产部署 README，以及 CLI、架构、协议和开发文档。
- `minitun`、`minitund` 和 `minitun-server` 可执行程序。
- 单元测试和 GitHub Actions CI 基础。

### 修复

- 为每个公网服务器会话创建独立的客户端 TLS 上下文，避免多服务器并发握手共享
  OpenSSL 证书验证缓存。
- 修复 GCC 11/12 下移动协议负载及分离式 TLS `GOAWAY` 写入的协程帧生命周期问题。
