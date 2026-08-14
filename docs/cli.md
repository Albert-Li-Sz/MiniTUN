# 命令行界面

`minitun` 是无状态、短生命周期客户端。它不打开 SQLite，也不直接连接公网 server；
每条命令只向本地 `minitund` Unix socket 发送一个 IPC envelope v1 请求。

全局选项：

```text
--socket <path>   daemon socket，默认 /run/minitun/minitun.sock
```

## daemon 与运行状态

```text
minitun daemon status
minitun daemon identity [--json]
minitun status [--json]
minitun health
minitun readiness
minitun metrics
minitun reload
minitun version
```

`daemon identity` 返回跨重启稳定的 `client_id`，用于 server 的 `clients.json`。daemon
readiness 检查状态库、凭据库和 IPC，不要求所有远端在线。`metrics` 的计数在进程重启后
归零。

## server 生命周期

```text
minitun server add <endpoint> [--name <name>]
minitun server login <id-or-name> [--psk-stdin]
minitun server update <id-or-name>
        [--name <name> | --clear-name]
        [--endpoint <host:port>]
        [--tls-server-name <name> | --clear-tls-server-name]
        [--ca-file <pem> | --clear-ca]
        [--client-cert <pem> --client-key <pem> | --clear-client-identity]
minitun server enable <id-or-name>
minitun server disable <id-or-name>
minitun server logout <id-or-name>
minitun server list [--json]
minitun server inspect <id-or-name> [--json]
minitun server remove <id-or-name>
```

`server login` 默认在 TTY 中显示 `PSK:` 并关闭回显。自动化必须显式从标准输入读取：

```bash
minitun server login edge --psk-stdin </secure/path/edge.psk
```

PSK 通过 `--psk-stdin` 或无回显 TTY 输入提供，不能作为
位置参数或普通 option，因此不会进入 shell history、进程参数或 `/proc`。JSON 响应只
返回 `credential_configured` 等布尔状态，不返回秘密或凭据引用。

endpoint、TLS server name、CA 或客户端证书身份变化只重建对应 server session。名称
变化不引起 session 抖动。`disable` 保留资源记录；`enable` 自动恢复同步。`logout`
删除该 server 的 PSK、CA、client cert 和 private key，并切换为未认证状态。

## tunnel 生命周期

```text
minitun tun add <server-id-or-name> <local-port> <server-port>
        [--local-host <host>] [--remote-host <numeric-host>]
        [--protocol tcp|udp|socks5|p2p] [--name <name>]
minitun tun update <tun-id-or-name>
        [--name <name> | --clear-name]
        [--local-host <host>] [--local-port <port>]
        [--remote-host <numeric-host>] [--server-port <port>]
        [--protocol tcp|udp|socks5|p2p]
minitun tun enable <tun-id-or-name>
minitun tun disable <tun-id-or-name>
minitun tun list [server-id-or-name] [--json]
minitun tun inspect <tun-id-or-name> [--json]
minitun tun remove <tun-id-or-name>
```

示例：

```bash
minitun tun add edge 8080 6000 --name web
```

表示 `公网 server 0.0.0.0:6000/tcp -> daemon 127.0.0.1:8080/tcp`。即使 server 离线，记录
也以 `desired_state=active`、`actual_state=pending` 创建。稳定 tunnel ID 和 server
归属不可更新；名称、本地地址/端口和公开端口可以更新。

四种 mode 的创建示例：

```bash
# 公网 UDP 6001 -> 本地 UDP 5353
minitun tun add edge 5353 6001 --protocol udp --name dns

# server loopback 上的 SOCKS5 CONNECT；local-port=1 只是兼容位置参数
minitun tun add edge 1 6002 --protocol socks5 \
  --remote-host 127.0.0.1 --name proxy

# P2P tunnel 的固定本地 TCP 目标
minitun tun add edge 8080 6003 --protocol p2p --name direct-web
minitun-p2p tunnel.example.com:6003 --listen 127.0.0.1:6501
```

`--remote-host` 是 server 侧的数值 bind address，默认 `0.0.0.0`。SOCKS5 强制使用
loopback bind，只支持 no-auth `CONNECT`（IPv4、IPv6、domain），不支持 BIND 或 UDP
ASSOCIATE。UDP 保留 datagram 边界，单个 payload 上限 65,507 字节。P2P 先尝试一次性
token 认证的 direct TCP candidate，失败时自动回退到 TLS relay；它不提供
ICE/STUN/TURN/NAT 打洞，也不额外加密 direct path。

修改公开端口时先撤销旧 listener，再注册新端口。新端口失败时资源保持新期望配置并
显示 `failed`，旧入口不会继续存在。disable 保留记录并注销 listener；enable 后重新
收敛。remove 删除记录并异步保证远端 listener 不再转发。

tunnel JSON 的关键诊断字段：

| 字段 | 含义 |
| --- | --- |
| `config_revision` | 单调递增的期望配置 revision |
| `server_actual_state` | 所属 server 当前实际状态 |
| `pending_reason` | `pending` 的稳定原因，否则为 `null` |
| `last_synced_at` | 最近匹配远端响应的 Unix 毫秒时间戳 |
| `last_error` | 最近一次稳定、非敏感同步错误 |

## 声明式配置

```text
minitun config export
minitun config plan <format-version-1.json> [--prune]
minitun config apply <format-version-1.json> [--prune]
```

plan 只读并稳定排序 create/update/disable/delete 动作。apply 默认不删除；只有
`--prune` 才删除此前由 apply 管理、但现在缺失的资源，命令式资源不受影响。全量预校验
和状态事务保证失败不出现半套资源；相同配置重复 apply 返回零动作，不重建 session。

`config export` 不输出凭据路径或内容，只标记是否已配置。完整格式见
[配置文档](configuration.md)。

## 数据库诊断、备份与恢复

```text
minitun doctor [--json] [--checkpoint]
       [--backup-state <path>] [--backup-credentials <path>]
       [--restore-state <path>] [--restore-credentials <path>]
```

生产升级前应在 daemon 在线时生成同一批次的 state/credentials 备份：

```bash
install -d -m 0700 /var/backups/minitun/pre-v1
minitun doctor --json --checkpoint \
  --backup-state /var/backups/minitun/pre-v1/state.db \
  --backup-credentials /var/backups/minitun/pre-v1/credentials.db
```

同时在线恢复时会先验证两个来源，再分别原子替换并唤醒同步；两个 SQLite 文件之间不是
同一个数据库事务，因此必须使用成对备份。降级到旧 schema 需要离线恢复成对备份。

## P2P connector

```text
minitun-p2p <server-host:tunnel-port>
  [--listen <numeric-host:port>] [--relay-only]
  [--connect-timeout <seconds>] [--negotiation-timeout <seconds>]
  [--direct-timeout <seconds>] [--inactivity-timeout <seconds>]
  [--allow-non-loopback]
```

默认监听 `127.0.0.1:6501`。每个本地连接独立协商并打印选中的 `direct` 或 `relay`
path；`--relay-only` 可用于策略控制和验证 fallback。只有明确要向受信网络暴露本地入口
时才使用 `--allow-non-loopback`。

## 输出与退出码

`status --json`、`doctor --json`、health/readiness/metrics/reload、config 命令输出 JSON
object；`list --json` 输出 array；`inspect --json` 输出单个 object。墓碑和内部凭据引用
不出现在公开结果。

| 代码 | 含义 |
| ---: | --- |
| `0` | 成功 |
| `2` | 参数无效、资源未知或冲突 |
| `3` | 本地 daemon 不可用或不可访问 |
| `4` | 认证失败 |
| `5` | 远程、TLS 或网络失败 |
| `10` | 协议、数据库、资源耗尽或内部失败 |

## daemon 关键选项

```text
minitund [--foreground]
  [--socket /run/minitun/minitun.sock]
  [--database /var/lib/minitun/state.db]
  [--credentials /var/lib/minitun/credentials.db]
  [--admin-listen <numeric-host:port>] [--admin-token-file <file>]
  [--tls-ca <pem>] [--relay-idle-timeout <seconds>]
  [--shutdown-timeout <seconds>]
  [--max-idle-workers-per-server <n>] [--max-total-idle-workers <n>]
  [--max-total-connections <n>] [--io-threads <1..16>]
```

`--insecure-skip-verify` 只用于本地开发，生产不得使用。
