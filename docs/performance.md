# 性能与浸泡门禁

正式证据只能来自没有其他工作负载的独立 Linux 主机：恰好 4 vCPU、约 8 GiB 内存、
`RLIMIT_NOFILE >= 65536`。GitHub Actions 的 `Performance release gate` 只调度带
`minitun-benchmark-4cpu-8gib` 标签的自托管 runner。该 runner 应为此工作流专用；浸泡
期间不得调度其他任务。

所有正式 JSON 使用 `evidence_format: 1`，记录完整 40 位 source commit、主机规格、墙钟
和单调时长、规模、吞吐、延迟、RSS、收敛时间及失败列表。工作流对核心 JSON 生成 GitHub
OIDC attestation；release 工作流只下载同一提交的成功工件并再次验证 attestation。

## 三次独立基准

本地等价命令如下；它只用于调试，正式门禁必须由专用 runner 执行：

```bash
cmake -S . -B build/performance -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINITUN_USE_SYSTEM_DEPS=ON \
  -DMINITUN_BUILD_TESTS=OFF \
  -DMINITUN_ENABLE_LTO=ON
cmake --build build/performance --parallel 4

MINITUN_SOURCE_SHA=$(git rev-parse HEAD) \
RESULT_DIR=benchmark-results RUNS=3 \
  benchmarks/run_gate.sh \
    build/performance/minitun \
    build/performance/minitund \
    build/performance/minitun-server
```

正式执行：

```bash
gh workflow run performance.yml --ref main -f operation=benchmark
```

每次运行都会重新创建 100 个 daemon、2,000 条 tunnel 和 10,000 条并发 relay，并测量
同机直连 TLS/TCP echo 基线。每条连接发送确定性数据并逐块校验回显。门禁检查：

- 三次中位有效载荷吞吐 ≥1 Gbit/s，且 ≥直连 TLS 基线的 85%；
- 每次的 10,000 relay 零损坏、零非配额拒绝，p95 首字节 ≤250 ms；
- MiniTun server 与 100 个 daemon 的聚合 RSS 峰值 ≤4 GiB；
- server 重启后 2,000 tunnel 在 30 秒内恢复。

结果写入 `run-1.json`…`run-3.json`、每轮脱敏日志和 `gate-summary.json`。若三轮聚合门禁
仍失败，RC 冻结前必须实现并重新验收 Protocol v2 `multiplexed_streams`；通过时 v1
继续使用一条 relay 对应一条 TLS Worker。

## 24 小时与 7 天连续浸泡

GitHub Actions 的单步、令牌和 job 生命周期短于完整 7 天。为保持同一批进程和连续 RSS
测量，`start-*` 操作会把已构建二进制与脚本复制到
`/var/lib/minitun-release-gates`，再启动持有主机级互斥锁的 systemd service。工作流结束
不会终止该 service；完成后用 start 工作流的数字 run ID 收集结果。收集过程会复核二进制
与脚本 SHA-256，拒绝运行中、失败、缩短或被替换的会话。

最终 RC 必须先发布。以下示例假设最终候选版是 `v1.0.0-rc.2`，且 GA 将指向完全相同的
commit。

先启动满规模 24 小时压力：

```bash
gh workflow run performance.yml --ref v1.0.0-rc.2 \
  -f operation=start-full-24h
```

记下这次 start 工作流的数字 run ID。至少 24 小时后收集；提前收集会失败：

```bash
gh workflow run performance.yml --ref v1.0.0-rc.2 \
  -f operation=collect-full-24h \
  -f session_id=<START_WORKFLOW_RUN_ID>
```

只有 24 小时工件通过后，才启动后续 7 天混合负载：

```bash
gh workflow run performance.yml --ref v1.0.0-rc.2 \
  -f operation=start-mixed-7d
# 七个完整自然日后：
gh workflow run performance.yml --ref v1.0.0-rc.2 \
  -f operation=collect-mixed-7d \
  -f session_id=<START_WORKFLOW_RUN_ID>
```

混合阶段在持续 10,000 连接批次之间执行 server 重启、2 秒断网窗口、策略 SIGHUP 和
逐客户端 PSK 轮换。任何 load 失败、进程崩溃、数据损坏或收敛超时都会使会话失败；稳定
阶段聚合 RSS 增长不得超过 5%。工件保留汇总、事件记录、server/daemon 日志、内核、CPU
和内存信息，不包含 PSK、私钥、数据库或用户载荷。

管理员可在专用 runner 查看状态：

```bash
sudo systemctl list-units 'minitun-soak-*'
benchmarks/soak_service.sh status \
  mixed-7d <40_HEX_COMMIT> <START_WORKFLOW_RUN_ID>
```

`SOAK_SECONDS_OVERRIDE` 仅用于本地脚本开发；正式 verifier 固定要求实际墙钟与单调时长
分别达到 86,400 秒和 604,800 秒，因此覆盖值不能生成可发布证据。

## GA 的不可绕过条件

推送 release tag 后，`release.yml` 在构建软件包前执行以下检查：

1. RC tag 连续且均为 annotated tag，GA 与最后一个 rc.2 或更高 RC 指向同一 commit；
2. 三轮基准、24 小时和 7 天工件都来自该 commit，且 attestation 有效；
3. 24 小时阶段开始于最终 RC 发布之后，7 天阶段开始于 24 小时阶段完成之后；
4. GitHub 中没有带 P0/P1 优先级标签的未关闭 issue；
5. 所有硬性性能、稳定性和资源门禁通过。

最终 RC 后任何协议、schema 或 SDK ABI 变化都会产生新 commit，因此 GA 的同提交检查会
拒绝发布；必须发布额外 rc.N，并在新 tag 后重新执行两个浸泡阶段。
