# 远程协议

MiniTun 使用受 TLS 保护的二进制协议。C++ 对象内存布局绝不会直接发送到网络；
每个整数和字段都显式编码为网络字节序。

## 帧格式

每条分帧消息都以固定的 24 字节头开始：

| 偏移 | 宽度 | 字段 | 值 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | `0x4D54554E`（`MTUN`） |
| 4 | 2 | version | `1` |
| 6 | 2 | message type | 下文所列值之一 |
| 8 | 4 | flags | 协议版本 1 中为 `0` |
| 12 | 4 | payload length | 头之后的无符号字节数 |
| 16 | 8 | request ID | 请求/响应关联标识符 |

完整头与负载合计限制为 64 KiB。当声明的帧超过该限制时，解码器会在分配负载存储
之前拒绝长度。允许空负载。增量解码支持任意 TCP 分片，也支持一次读取多帧。

## 消息类型

控制连接使用 `HELLO`、`HELLO_ACK`、`AUTH`、`AUTH_OK`、`AUTH_ERROR`、
`REGISTER_TUNNEL`、`REGISTER_TUNNEL_OK`、`REGISTER_TUNNEL_ERROR`、
`UNREGISTER_TUNNEL`、`UNREGISTER_TUNNEL_OK`、`REQUEST_WORKERS`、`PING`、
`PONG`、`GOAWAY` 和 `ERROR`。

Worker 连接使用 `WORKER_HELLO`、`WORKER_ACCEPTED`、`START_RELAY`、
`LOCAL_CONNECT_OK` 和 `LOCAL_CONNECT_ERROR`。收到 `LOCAL_CONNECT_OK` 后，连接
永久离开分帧模式，直至关闭都只承载原始 TCP 字节。

## 负载字段

负载整数使用网络字节序。字节串与 UTF-8 字符串使用两字节长度，后接精确数量的
字节。字符串拒绝畸形 UTF-8 和嵌入的 NUL 字节。除全帧上限外，每个解码器还会应用
字段专用上限。

## 状态校验

客户端与服务端状态机分别校验控制连接和 Worker 握手。认证完成后不能重放握手消息；
控制消息不能出现在 Worker 连接，Worker 消息不能出现在控制连接；切换为原始中继后
不再接受任何分帧消息。协议违规只关闭发生违规的连接。

## TLS 与认证

全部远程帧都在 TLS 内传输。服务端要求 TLS 1.2 或更高版本，禁用压缩和重新协商，
加载匹配的 PEM 证书/私钥对，并且绝不提供明文回退。客户端使用 SNI，并根据配置的
CA 包或平台信任库执行主机名校验。

控制握手如下：

```text
client  -> HELLO(client_id)
server  -> HELLO_ACK(server_id, server_time, 32-byte nonce)
client  -> AUTH(client_id, timestamp, nonce, authentication_data)
server  -> AUTH_OK(session_generation, heartbeat and worker limits)
       or AUTH_ERROR(authentication_failed)
```

`authentication_data` 是以下内容的 HMAC-SHA256：协议版本、带长度前缀的客户端 ID、
编码为 64 位网络字节序的有符号 Unix 时间戳，以及带长度前缀的服务端 nonce。Token
作为 HMAC 密钥，绝不会发送。服务端检查时钟偏差，通过有界重放缓存消费每个质询
nonce，以常量时间比较 HMAC 输出，并按对端地址限制失败速率。失败响应不会泄露 ID、
时间戳、nonce 或摘要中的哪一项有误。

每次认证成功都会替换客户端先前的随机 64 位 `session_generation`；代次零无效。
随后服务端发送带序号的 `PING`，并要求在有界心跳期限前收到匹配的 `PONG`。
服务端在该不透明序号中使用带标记的有界位段通告 `1-300` 秒 Worker 空闲期限；旧版
客户端仍会原样回显整个序号，新版客户端则解码期限并增加 5 秒关闭宽限，因此无需
提升协议版本即可保持双向兼容。没有标记的旧版服务端心跳不会被误作协商结果。

`GOAWAY` 在协议版本 1 中负载为空，可由任一已认证控制对端在有序关闭期间发送。
收到后会结束控制面状态同步并阻止创建新 Worker。已分配的原始中继是独立连接，可以
排空至本地优雅关闭期限；若期限或故障导致 `GOAWAY` 无法送达，对端仍必须容忍传输
立即关闭。

## 隧道注册

已认证控制连接会同步每条本地持久化的活动隧道：

```text
client -> REGISTER_TUNNEL(tunnel_id, bind_host, bind_port)
server -> REGISTER_TUNNEL_OK(tunnel_id)
       or REGISTER_TUNNEL_ERROR(tunnel_id, error_code)

client -> UNREGISTER_TUNNEL(tunnel_id)
server -> UNREGISTER_TUNNEL_OK(tunnel_id)
```

客户端绝不发送 `local_host` 或 `local_port`；这些配置只在本地持久化。服务端只接受
数值公网绑定地址，绑定前应用 `--allow-ports` 范围，限制每个已认证客户端的隧道数，
并将操作系统地址冲突映射为 `remote_port_in_use`。请求 ID 关联每个响应。注册和删除
均为幂等操作；全部监听器都属于一个客户端会话代次，因此陈旧控制会话无法继续持有它们。

## Worker Pool

认证完成后，`minitund` 会为该服务器会话预连接最低数量的 TLS Worker。Worker 使用
`client_id`、当前 `session_generation` 和随机 Worker ID 标识自己。服务端只接受
当前已认证控制连接拥有的代次：

```text
client -> WORKER_HELLO(client_id, session_generation, worker_id)
server -> WORKER_ACCEPTED(worker_id)

server -> REQUEST_WORKERS(count)  # 容量不足时在控制连接上发送
```

空闲 Worker 同时受会话级和全局上限约束，服务端默认 60 秒后使其过期，并通过心跳
向新版客户端通告实际配置；客户端关闭期限比服务端多 5 秒。Worker 在所属控制代次
关闭时会立即移除。公网连接等待匹配 Worker 最长两秒。分配时发送
`START_RELAY(tunnel_id, connection_id)`；本地目标绝不会进入线协议。

## 原始 TCP 中继

守护进程只根据自身 SQLite 中的活动记录解析 `tunnel_id`，连接已持久化的本地端点，
并回复 `LOCAL_CONNECT_OK(connection_id)` 或通用的
`LOCAL_CONNECT_ERROR(connection_id, local_connect_failed)`。成功帧之后，双方
永久离开分帧模式。每个方向读入一个固定 16 KiB 缓冲区，必须完成当前写入后才能
继续读取，从而提供有界内存和自然背压。一个方向收到 EOF 时，会对另一套接字执行
`shutdown_send`，同时允许反向流继续。空闲期限会取消双方；每条完成的中继在内部
报告持续时间和双向字节数。

中继准入限制在等待 Worker 前生效。公网服务端持有逐客户端与全局配额租约，直至
已分配连接结束；守护进程则把每条 Worker 连接（包括已消耗 Worker）都计入全局上限。

## 兼容性与演进

当前协议版本为 1。接收方会拒绝不支持的版本、未知消息类型、非零保留 flags、无效
状态转换、超长帧或超长字段，而不会尝试宽松解析。任何未来版本都必须继续使用显式
版本协商与有界字段，不得依赖编译器 ABI、结构体布局或主机字节序。
