# Remote Protocol v2

MiniTun 只接受 Remote Protocol v2；不存在更旧协议的兼容回退。
全部远程消息都在 TLS 1.2+ 内传输；不存在明文回退。C/C++ 对象布局不会直接上网，所有
整数、长度和字段都使用显式网络字节序编码。

## 帧格式

每条控制或 Worker 握手消息以固定 24 字节头开始：

| 偏移 | 宽度 | 字段 | v2 值 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | `0x4D54554E`（`MTUN`） |
| 4 | 2 | version | `2` |
| 6 | 2 | message type | 下文定义的消息类型 |
| 8 | 4 | flags | 当前必须为 `0` |
| 12 | 4 | payload length | 头之后的无符号字节数 |
| 16 | 8 | request ID | 请求/响应关联标识；零值仅用于不需要关联的消息 |

单帧上限为 64 KiB。解码器在分配 payload 前拒绝超长声明，支持任意 TCP 分片和一次
读取多个帧，并严格拒绝未知类型、非零保留 flags、畸形 UTF-8、嵌入 NUL、尾随字节和
非法状态转换。

客户端的注册同步窗口最多包含 32 个未完成请求。窗口内帧合并为一次有界 TLS application
write；响应可乱序到达，通过 request ID、tunnel ID 和 desired revision 关联。

## 能力协商与认证

控制握手：

```text
client  -> HELLO(client_id, offered_capabilities)
server  -> HELLO_ACK(server_id, server_time, nonce, selected_capabilities)
client  -> AUTH(client_id, timestamp, nonce, selected_capabilities, HMAC)
server  -> AUTH_OK(session_generation, heartbeat_interval, worker_limits)
       or AUTH_ERROR(authentication_failed)
```

v2 定义以下 capability bits：

| 能力 | 位值 | 状态与用途 |
| --- | --- | --- |
| `pipelined_control` | `1ULL << 0` | 必需 |
| `per_client_policy` | `1ULL << 1` | 必需 |
| `tunnel_revisions` | `1ULL << 2` | 必需 |
| `client_certificate_binding` | `1ULL << 3` | 双方支持，按策略使用 |
| `multiplexed_streams` | `1ULL << 4` | 已保留，但默认不支持也不通告 |
| `udp_datagrams` | `1ULL << 5` | 双方支持，使用 UDP mode 时必需 |
| `socks5_proxy` | `1ULL << 6` | 双方支持，使用 SOCKS5 mode 时必需 |
| `p2p_rendezvous` | `1ULL << 7` | 双方支持，使用 P2P mode 时必需 |
| `tcp_simultaneous_open` | `1ULL << 8` | 支持 server 辅助的 P2P TCP 打洞，允许 `START_RELAY` 携带公网 peer 来源端点 |
| `proxy_protocol` | `1ULL << 9` | 允许 `START_RELAY` 携带公网来源端点，供开启 `proxy_protocol` 的 TCP tunnel 生成 PROXY protocol v1 头 |
| `worker_observed_endpoint` | `1ULL << 10` | 在来源端点扩展后追加 server 观测到的 Worker 自身 IP，供 daemon 广告 NAT 可达的 P2P candidate |

server 只能选择客户端提供且自己支持的集合，并必须包含三个必需能力。控制认证摘要是以
该 client 的 PSK 为密钥的 HMAC-SHA256，输入绑定：协议版本、client ID、server ID、
时间戳、32 字节随机 nonce 和最终 capability 集合。PSK 与摘要都不会以明文日志输出。

server 还检查有界时钟偏差、nonce 重放缓存和来源限速，并以常量时间比较摘要。未知、
禁用、证书不匹配或 PSK 错误均得到同一种非敏感认证失败；实现仍执行等价 HMAC 路径，
避免通过错误文本或明显的早退区分策略存在性。

策略可额外绑定客户端证书 SHA-256 指纹或 SAN。此时 TLS 链必须通过 `--client-ca`
验证，控制连接和其 Worker 都必须匹配同一 client 策略；证书不能替代 PSK。

每次控制认证成功都会生成新的非零 64 位 `session_generation`。旧 generation 的
listener、Worker 或响应不得修改当前状态。心跳使用匹配的 `PING/PONG` 序号，并在有界
位段中通告 Worker 空闲期限。

## 隧道注册

```text
client -> REGISTER_TUNNEL(tunnel_id, bind_host, bind_port, desired_revision[, mode])
server -> REGISTER_TUNNEL_OK(tunnel_id, desired_revision)
       or REGISTER_TUNNEL_ERROR(tunnel_id, error_code, desired_revision)

client -> UNREGISTER_TUNNEL(tunnel_id, desired_revision)
server -> UNREGISTER_TUNNEL_OK(tunnel_id, desired_revision)
```

`local_host` 和 `local_port` 永远不进入远程协议；server 只看到公开绑定和 transport
mode。TCP 沿用原有 payload，不追加 mode 字节；UDP、SOCKS5 和 P2P 分别追加一个显式
mode byte，并要求当前 session 已协商对应 capability。注册前会检查数值 bind address、
客户端 ACL、逐客户端/全局 tunnel 配额和操作系统绑定结果。daemon 和 server 均强制
SOCKS5 的 bind address 为数值 loopback；server 在创建 listener 前拒绝通配地址、
非 loopback IP 和主机名，即使客户端绕过 daemon 直接发起注册也无法跳过此检查。

daemon 只有在 session generation、frame request ID、tunnel ID 和当前
`config_revision` 全部匹配时才提交响应。重复响应和旧响应被忽略。超时或断线会把该
generation 遗留的 `registering`、`removing`、`active` 状态收敛到 `pending`。

修改公开端口或地址时，daemon 先发送旧 listener 的 unregister，收到对应确认后才注册
新 desired revision。新端口注册失败时保留新的期望配置并标为 `failed`，不会恢复旧入口。

## Worker 与 relay

Worker 使用单独 TLS 连接：

```text
client -> WORKER_HELLO(client_id, generation, worker_id, timestamp, nonce, HMAC)
server -> WORKER_ACCEPTED(worker_id)

server -> REQUEST_WORKERS(count)                 # 控制连接
server -> START_RELAY(tunnel_id, connection_id[, mode]) # 已分配 Worker
client -> LOCAL_CONNECT_OK(connection_id)
       or LOCAL_CONNECT_ERROR(connection_id, local_connect_failed)
```

Worker HMAC 绑定协议版本语境下的 client ID、server ID、generation、worker ID、时间戳和
nonce。server 要求当前控制 generation、同一策略、同一 PSK 和同一证书绑定。

公网连接在等待 Worker 前占用逐客户端和全局 connection quota。空闲 Worker 数同时受
客户端策略、server 上限和 daemon 上限控制，并按需求在最小值和最大值之间自适应补充。

未使用端点扩展时，TCP `START_RELAY` 保持既有 wire image；非 TCP mode 追加同一
枚举字节。Worker 继承所属控制 session 的 capability 集合；协商
`proxy_protocol` 或 `tcp_simultaneous_open` 后，server 可追加公网连接的
`source_host` 和 `source_port`。这个扩展总是先写显式 mode 字节，包括 TCP mode。
两字段必须成对存在，host 为数值 IP，port 为非零 `uint16`。仅在来源端点扩展存在且
另行协商 `worker_observed_endpoint` 时，server 才继续追加 `worker_observed_host`
（Worker TLS 连接的 server 观测 IP，不含端口）。未协商扩展的旧对端仍收到原有编码。

来源端点本身不会开启 PROXY 头；只有 TCP tunnel 的本地 `proxy_protocol` 配置为
`true` 时，daemon 才在目标连接上添加 PROXY protocol v1 头。

收到 `LOCAL_CONNECT_OK` 后，该 Worker 永久离开 Remote Protocol 帧模式，只承载一条数据面：

| mode | Worker 数据面 |
| --- | --- |
| `tcp` | 原始 TLS application bytes；daemon 连接固定本地 TCP 目标。 |
| `udp` | `uint16` 大端 payload length + 0..65,507 字节 payload；每条 record 保留一个 UDP datagram 边界。 |
| `socks5` | TLS 内的 SOCKS5 no-auth CONNECT 握手，支持 IPv4/IPv6/domain；成功后为原始 TCP bytes。 |
| `p2p` | 一次性 token 和 direct candidate 协商；成功后改走 TLS 1.3 加密的直连 TCP，否则确认并继续 TLS relay。 |

TCP relay 每个方向使用固定 16 KiB 缓冲，写完当前块后才继续读，形成有界
backpressure。EOF 传播为 half-close，反向数据可以继续；空闲超时或 reset 会关闭该
relay，并恰好释放一次 quota。UDP 的公网 peer session、待发送 datagram 数量和总字节数
也有界，空闲后自动释放。

P2P candidate 以随机一次性 token 认证并绑定单次协商。候选认证后，两端将 direct
socket 升级为 TLS 1.3，以 token 作为 external PSK 加密应用数据；direct context
使用统一的显式 TLS 套件及禁压缩、禁重协商策略，且只允许 TLS 1.3。
协商 `tcp_simultaneous_open` 且 connector 开启该选项时，普通 direct 失败后可尝试
server 辅助的 TCP 打洞，支持双 EIM NAT 穿透；不实现 ICE、STUN、TURN 或 UDP 打洞。
无法连接或确认时自动回退到原认证 TLS Worker。

## 重载与关闭

服务端完整校验新 TLS/策略快照后才原子切换。被删除、禁用或有效策略发生变化的 client：

1. 立即撤销 listener，停止接受新 relay；
2. 关闭控制连接和空闲 Worker；
3. 已分配 relay 最多排空 `--shutdown-timeout`，随后强制关闭。

未变化 client 的 session 不因纯策略重载抖动。纯 server TLS 凭据重载只影响新连接和
控制/空闲 Worker 的轮换，不中断正在传输的 relay。

`GOAWAY` 可由任一已认证控制对端发送。进程退出时先停止 listener 和新分配，再在同一
优雅期限内排空 relay；故障情况下对端必须容忍传输直接关闭。

## 兼容性边界

- 所有 1.x 版本只接受协议号 2，不尝试降级或宽松解析 v1。
- TCP REGISTER 保留原编码；未协商端点扩展时，TCP START payload 与 1.0 完全相同。
  新 mode 和端点元数据只在双方协商对应 capability 后追加，因此 1.0 对端不会收到
  无法解析的非 TCP tunnel 或端点扩展。
- 仍保持“一条 relay 一条 TLS Worker”，不实现 `multiplexed_streams`。
- `libminitun-remote-protocol.so.1` 暴露与 wire codec 共用的强类型 message variant、
  增量 frame decoder、frame/message codec 和 control/Worker 认证摘要 helper；它不创建
  socket、TLS session、daemon 或 server runtime。
