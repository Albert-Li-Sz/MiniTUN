# 配置与客户端策略

MiniTun v1 有两份严格 JSON：公网服务端的客户端策略，以及每个 `minitund` 的声明式
资源配置。两者都拒绝重复键、未知字段、错误类型和超出上限的值；只有完整文件校验成功
后才会切换当前配置。

## 服务端客户端策略

`minitun-server` 必须通过 `--clients-config` 指定策略文件。每个客户端都有稳定
`client_id` 和独立 PSK；端口 ACL、隧道、连接与空闲 Worker 配额也按客户端配置。

```json
{
  "format_version": 1,
  "clients": [
    {
      "client_id": "client_0123456789abcdef0123456789abcdef",
      "enabled": true,
      "psk_file": "/etc/minitun-server/clients/team-a.psk",
      "allowed_ports": ["6000-6099", "8443"],
      "max_tunnels": 100,
      "max_connections": 1000,
      "max_idle_workers": 32,
      "allowed_source_cidrs": ["203.0.113.0/24", "2001:db8::/32"],
      "connections_per_minute": 60,
      "certificate_san": "URI:spiffe://example.internal/minitun/team-a"
    }
  ]
}
```

`certificate_san` 与 `certificate_sha256` 二选一，也可以都省略。启用任一证书绑定时：

- 服务端必须配置 `--client-ca`；
- 控制连接和 Worker 都必须提交由该 CA 验证的证书；
- 证书还必须匹配策略中的 SAN 或小写十六进制 SHA-256 指纹；
- PSK 仍然必需，证书不能替代 PSK。

`allowed_ports` 是闭区间数组，只限制公网监听端口。范围不得重叠；拒绝会记录有界审计
事件，但不会把客户端或隧道名称变成指标标签。

两个可选字段控制公开端口的来源准入：

- `allowed_source_cidrs`：CIDR 白名单（IPv4/IPv6，1–64 条）。存在时只有来自这些
  网络的公开连接被接受；缺省为空，允许所有来源。
- `connections_per_minute`：每来源 IP 的连接速率上限（每分钟，最大 1000000）。
  缺省为 0，表示不限速。

被来源策略拒绝的连接计入 `minitun_source_rejections_total` 指标。

PSK 文件必须为当前服务账户拥有的普通文件，且组用户和其他用户不可访问。读取时会
规范化末尾 CR/LF。证书和策略文件可以由组读取，但不得由组或其他用户写入。

安全地重载策略：

```bash
sudo install -m 0640 -o minitun-server -g minitun-server \
  clients.json.new /etc/minitun-server/clients.json.new
sudo mv /etc/minitun-server/clients.json.new /etc/minitun-server/clients.json
sudo systemctl kill -s HUP minitun-server.service
```

解析或校验失败时保留旧快照。被禁用、删除或凭据发生变化的客户端停止接收新流量，
活动 relay 在优雅期限内排空，然后控制连接和空闲 Worker 断开。未变化客户端不会抖动。

## 声明式资源配置

本地配置使用 `format_version: 1`，包含 `servers` 和 `tunnels`：

```json
{
  "format_version": 1,
  "servers": [
    {
      "name": "edge",
      "endpoint": "tunnel.example.com:2333",
      "tls_server_name": "tunnel.example.com",
      "psk_file": "secrets/edge.psk",
      "ca_file": "secrets/organization-ca.pem",
      "client_cert_file": "secrets/client-chain.pem",
      "client_key_file": "secrets/client-key.pem",
      "enabled": true
    }
  ],
  "tunnels": [
    {
      "name": "web",
      "server": "edge",
      "protocol": "tcp",
      "local_host": "127.0.0.1",
      "local_port": 8080,
      "remote_host": "0.0.0.0",
      "remote_port": 6000,
      "enabled": true
    },
    {
      "name": "dns-udp",
      "server": "edge",
      "protocol": "udp",
      "local_host": "127.0.0.1",
      "local_port": 5353,
      "remote_port": 6001,
      "enabled": true
    },
    {
      "name": "private-proxy",
      "server": "edge",
      "protocol": "socks5",
      "remote_host": "127.0.0.1",
      "remote_port": 6002,
      "enabled": true
    },
    {
      "name": "p2p-web",
      "server": "edge",
      "protocol": "p2p",
      "local_host": "127.0.0.1",
      "local_port": 8080,
      "remote_port": 6003,
      "enabled": true
    }
  ]
}
```

Tunnel 字段规则：

| 字段 | 规则 |
| --- | --- |
| `protocol` | 可选，默认 `tcp`；可为 `tcp`、`udp`、`socks5`、`p2p`。 |
| `local_host` | 可选，默认 `127.0.0.1`；SOCKS5 mode 忽略。 |
| `local_port` | TCP、UDP、P2P 必需；SOCKS5 可省略。 |
| `remote_host` | 可选；TCP/UDP/P2P 默认 `0.0.0.0`，SOCKS5 默认且只允许数值 loopback。 |
| `remote_port` | 必需，范围 1..65535，并受服务端 `allowed_ports` 约束。 |

SOCKS5 只实现 no-auth CONNECT；把它限制在 server loopback 是强制安全边界。P2P 适合
LAN 或已有可路由路径，不包含 ICE/STUN/TURN/NAT 打洞，direct path 也不附加传输加密；
协商失败会自动回退到 TLS relay。

相对凭据路径以配置文件所在目录为基准。`plan` 完全只读，动作按资源类型与稳定键排序：

```bash
minitun config plan /etc/minitun/config.json
minitun config apply /etc/minitun/config.json
```

匹配规则是：先按稳定 ID；没有 ID 时，server 按唯一名称，tunnel 按同类型唯一名称。
已有 tunnel 的 ID 和 server 归属不能改变。重复 apply 相同文件返回零动作，也不会重建
远程 session。

默认 apply 只创建和更新。显式 `--prune` 才删除由此前 apply 管理、但本次缺失的资源；
命令式创建的资源永远不被 prune：

```bash
minitun config plan /etc/minitun/config.json --prune
minitun config apply /etc/minitun/config.json --prune
```

apply 会先完整解析所有资源和凭据、验证 TLS 材料，再暂存新秘密，并在一个状态事务中
切换资源与凭据引用。失败会清理暂存项；守护进程启动时还会清理崩溃留下且不可达的凭据。

`config export` 不包含路径或秘密，只导出凭据是否已配置的布尔标记。导出的标记可用于
审阅；重新 apply 时若不提供对应 `*_file`，会保留当前凭据。
