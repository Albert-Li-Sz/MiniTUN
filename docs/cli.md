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
minitun daemon status
minitun version
minitun help
```

所有命令都接受以下全局覆盖选项：

```text
--socket <path>   本地 minitund 套接字（默认 /run/minitun/minitun.sock）
```

`list --json` 输出 JSON 数组，`inspect --json` 输出单个 JSON 对象。服务器 JSON
只包含 `credential_configured`，绝不暴露凭据引用或 Token。列表和详情命令不会
返回已经删除的墓碑记录。

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
