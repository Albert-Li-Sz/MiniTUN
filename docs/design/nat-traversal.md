# P2P NAT 打洞设计与实现

> 状态：v1.1 已实现 TCP simultaneous open（server 辅助，无 STUN/TURN）。验证基于
> loopback 与单元级 simultaneous open 拓扑；真实 NAT 组合的穿透率仍需按下方验收
> 标准实测。本文档描述协议设计、触发条件与剩余验收项。

## 现状

P2P direct path 只适用于「双方可路由」的场景：daemon 把自己的 Worker 对端地址
（server 视角看到的地址）作为 direct candidate 交给 peer。任一方向存在 NAT、且
没有端口转发时，candidate 不可达，peer 自动回退到认证 TLS relay。这正是「内网
穿透」定位下最大的功能缺口。

## 目标

在不改变 TCP wire image、不引入 STUN/TURN 基础设施、不显著增加资源占用的前提下，
让最常见的 NAT 组合（两端均为 Endpoint-Independent Mapping）可以通过 TCP
simultaneous open（TCP SO）直接互通；失败时仍回退 relay。

明确不做：

- 不做 STUN/TURN/ICE 完整栈——维持最小资源占用与单 server 架构；
- 不做 UDP 打洞——UDP 仍走认证 Worker 的中继路径；v1.1.1 起 P2P 路径本身可选
  承载带 2 字节长度前缀的 UDP datagram record（direct 与 relay 路径皆可），
  仍保持「一 relay 一认证 Worker」模型，不引入新端口；
- 不引入新的对外端口或监听器——复用现有公开 tunnel listener。

## 方案：server 辅助的 TCP simultaneous open

### 1. 观测地址交换（协议扩展）

`START_RELAY` 的 p2p 分支已经携带 tunnel/connection ID。新增两个可选字段：

- `peer_observed_endpoint`：server 观测到的 peer 公开地址（server 在 peer 的
  bootstrap TCP 连接上得到）；
- `worker_observed_endpoint`：server 观测到的 daemon Worker 公开地址（server 在
  Worker 的 TLS 连接上得到，即现有 direct candidate）。

双方在「对称 NAT 预判」后各自以**同一本地端口**向对方的观测地址发起 outbound
connect（SO_REUSEADDR 绑定到与 listening/worker socket 相同的本地端口），同时
保持原有 inbound accept 与 relay 回退。TCP SO 无需新增 wire 帧：成功后两端看到的
就是一条普通 TCP 连接，随后走现有 MTPD + token + TLS-PSK 升级。

### 2. 触发条件与回退

- 仅当 `mode == p2p` 且 peer 在 `--direct-timeout` 内普通 connect 失败时尝试 SO；
- SO 与 relay 回退并行竞速，谁先建立谁胜出；direct 建立后立刻关闭 relay 侧；
- 一端不支持该扩展（无 capability）时行为与今天完全一致。

### 3. Capability 协商

新增 `tcp_simultaneous_open` capability 位（`1ULL << 8`）。双方都宣告后才发送
观测地址字段；旧实现忽略未知 capability，保持 wire 兼容。

### 4. 已实现与剩余验证

v1.1 按本设计落地了 SO 数据面：peer 在普通 direct 失败后经 relay 发送 `MTPS` 请求，
daemon 从 direct listener 的同一本地端口向 peer 的 server 观测地址发起 outbound
connect，双方 connect 交叉后走既有 `MTPD` + token + TLS 1.3 PSK 升级；失败按原路径
回退 relay。peer 侧以 200ms 步长在 `--direct-timeout` 窗口内重试，兼容「首个 SYN
先于对端映射建立」的丢包窗口。capability `tcp_simultaneous_open`（`1ULL << 8`）
由 daemon↔server 协商；connector 默认开启 `--simultaneous-open`，对接 v1.0 daemon
时需 `--no-simultaneous-open`（旧 host 会把 `MTPS` 视为非法控制帧）。

单元测试覆盖：loopback 双侧 simultaneous open（无 listener）、SO socket 与 listener
同端口绑定及降级、peer 协议全流程（direct 失败→`MTPS`→SO 建立→`MTPD` 握手→TLS
失败→relay 回退确认）。真实 NAT 穿透率尚未实测。

### 5. 验收标准（剩余门槛）

1. 双 EIM NAT 下 direct 建立成功，且数据路径走 TLS-PSK 加密（与现状一致）；
2. 对称 NAT、无 NAT、单侧 NAT 的组合下 relay 回退时间不劣于现状；
3. 复用同一本地端口绑定 SO 的端口在 server 重启、generation 变化后正确释放；
4. 新增 e2e 测试通过真实 NAT 环境（或可控的 netns/iptables 拓扑）验证；
5. 审计日志记录选择的路径与失败原因，不记录任何地址之外的敏感信息。

## 备选路线（记录在案，不采纳）

- **QUIC 打洞**：穿透率更高，但引入完整 QUIC 栈，与「资源占用最小」冲突。
- **部署中继 TURN**：server 支持 TURN 中继可覆盖对称 NAT，但把 server 变成带宽
  汇聚点，改变安全与容量模型。
- **IPv6 优先**：引导用户使用 IPv6（无 NAT）作为「零成本打洞」，可在文档中先行
  推荐；这不排斥未来的 SO 方案。
