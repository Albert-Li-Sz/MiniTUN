# 命令行界面

`minitun` 是无状态的短生命周期客户端。它从不打开 SQLite，也不直接连接公网
MiniTun 服务端；每条资源命令只向本地 `minitund` Unix 套接字发送一个请求，打印
响应后退出。

## 命令

```text
minitun server add <server-endpoint> [--name <name>]
minitun server login <server-id-or-name> [--token-stdin]
minitun server list [--json]
minitun server inspect <server-id-or-name> [--json]
minitun server remove <server-id-or-name>

minitun tun add <server-id-or-name> <local-port> <server-port>
                [--local-host <host>] [--name <name>]
minitun tun list [server-id-or-name] [--json]
minitun tun inspect <tun-id-or-name> [--json]
minitun tun remove <tun-id-or-name>

minitun status
minitun doctor [--json] [--checkpoint]
               [--backup-state <path>] [--backup-credentials <path>]
               [--restore-state <path>] [--restore-credentials <path>]
minitun health
minitun readiness
minitun metrics
minitun reload
minitun daemon status
minitun version
minitun help
```

所有命令都接受以下全局覆盖选项：

```text
--socket <path>   本地 minitund 套接字（默认 /run/minitun/minitun.sock）
```

`status --json`、`doctor --json`、`health`、`readiness`、`metrics` 和 `reload` 输出 JSON 对象；
`list --json` 输出 JSON 数组，`inspect --json` 输出单个 JSON 对象。服务器 JSON
只包含 `credential_configured`，绝不暴露凭据引用或 Token。列表和详情命令不会
返回已经删除的墓碑记录。

隧道 JSON 还包含以下诊断字段：

| 字段 | 含义 |
| --- | --- |
| `server_actual_state` | 关联服务器当前状态；关联记录不存在时为 `null` |
| `pending_reason` | 隧道为 `pending` 时的稳定原因标识，否则为 `null` |
| `last_synced_at` | 最近一次远端注册或注销响应的 Unix 毫秒时间戳；从未同步时为 `null` |

## Token 输入

未指定 `--token-stdin` 时，`server login` 要求交互式终端，并在读取一行 Token
期间关闭回显：

```text
Token:
```

自动化场景必须显式选择从标准输入读取一行：

```bash
printf '%s\n' "$MINITUN_TOKEN" |
  minitun server login primary --token-stdin
```

Token 不能作为位置参数或普通选项传入，因此不会进入 Shell 历史、进程参数或
`/proc`。守护进程在本地保存凭据、唤醒对应的状态同步会话，并在不向 CLI 返回
秘密的情况下使用它进行远程认证。

## 隧道语义

默认命令：

```bash
minitun tun add primary 22 6000
```

会持久化以下期望 TCP 路由：

```text
公网服务端 0.0.0.0:6000 -> 本地客户端 127.0.0.1:22
```

即使公网服务端离线，记录仍会以 `desired_state=active`、`actual_state=pending`
创建。也可以指定自定义本地目标和显示名称：

```bash
minitun tun add primary 8080 6001 \
  --local-host 192.168.1.10 \
  --name nas-web
```

`tun remove` 会在返回成功前物理删除本地状态行，因此名称和端口可以立即重新创建。
控制面提交后会立即唤醒对应服务器会话并注销公网监听器，心跳同步只作为兜底；删除
完成后到注销之间的新连接不会再转发到本地目标。`server remove` 同样先提交状态删除，
再清理独立凭据库，避免活动状态引用已经删除的 Token。

## 本地运维命令

`health` 和 `readiness` 用于服务管理和本地探测。二者只访问本地 Unix 套接字，
不会增加公网监听面：

```bash
minitun health
minitun readiness
```

`metrics` 返回本地运行指标，包括活动会话、空闲与活动 Worker、连接配额、重连次数、
持久化/协议错误计数、Worker 配额拒绝次数和成功中继的双向字节数。客户端侧没有公网
排队语义，因此 `connections.pending` 当前为 `0`：

```bash
minitun metrics
minitun status --json
```

`reload` 会请求 `minitund` 重新创建远程会话，使 CA、Token 和会话配置在不重启
守护进程的情况下生效；已建立的中继按优雅关闭期限排空：

```bash
minitun reload
```

`doctor` 用于本地 SQLite 诊断、WAL checkpoint、在线备份和受控恢复。备份与恢复路径
必须位于真实、私有且受保护的目录中：

```bash
minitun doctor --json --checkpoint
minitun doctor --json \
  --backup-state /var/backups/minitun/state.db \
  --backup-credentials /var/backups/minitun/credentials.db
```

恢复操作会在线替换相应数据库内容，并唤醒状态同步。同时恢复两个数据库时，守护进程
会在修改任一实时数据库前完整验证两个来源；每个 SQLite 文件独立原子替换，但两个文件
之间不构成同一数据库事务。生产恢复前应同时备份 `state.db` 与 `credentials.db`，并使用
同一时间点生成的成对备份，避免状态记录和凭据引用不一致。

## 退出码

| 代码 | 含义 |
| ---: | --- |
| `0` | 成功 |
| `2` | 参数无效、资源未知或资源冲突 |
| `3` | 本地守护进程不可用或不可访问 |
| `4` | 认证失败 |
| `5` | 远程或网络失败 |
| `10` | 协议、数据库、资源或内部失败 |

CLI11 的帮助/版本控制流也以 `0` 退出；其他所有解析失败统一规范为 `2`。

## 守护进程选项

```text
minitund [--foreground]
          [--socket /run/minitun/minitun.sock]
          [--database /var/lib/minitun/state.db]
          [--credentials /var/lib/minitun/credentials.db]
```

`minitund` 会打开并迁移两个数据库、归一化重启状态、确认每个已持久化凭据引用都
存在，然后启动 IPC。父目录必须事先存在，并归守护进程账户所有或受到相应保护。
