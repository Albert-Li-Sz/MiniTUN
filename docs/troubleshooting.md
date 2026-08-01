# 故障排查

本文档提供 MiniTun 常见运行问题的安全诊断步骤。执行命令前请确认路径和服务名称与
实际安装方式一致。提交问题时遵循[支持说明](../SUPPORT.md)，切勿公开 Token、私钥、
完整凭据数据库或未经脱敏的内部信息。

## 基础诊断信息

首先记录版本、服务状态和最近日志：

```bash
minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
systemctl status minitund.service minitun-server.service
journalctl -u minitund.service -u minitun-server.service --since '-10 min'
```

检查本地控制面：

```bash
minitun daemon status
minitun status
minitun server list --json
minitun tun list --json
```

日志中可能包含地址、端口、资源 ID 和错误码。分享前应脱敏；MiniTun 不应输出 Token，
如果发现疑似秘密泄漏，请停止公开传播并按[安全策略](../SECURITY.md)报告。

## `minitund` 无法启动

### 父目录不存在或权限错误

生产环境中的 `/run/minitun` 和 `/var/lib/minitun` 由 systemd 的
`RuntimeDirectory`/`StateDirectory` 创建。不要以普通用户手工创建生产目录。

```bash
systemctl cat minitund.service
systemctl restart minitund.service
namei -l /run/minitun/minitun.sock
namei -l /var/lib/minitun/state.db
```

源码开发时，请把套接字和数据库放入当前用户所有、模式为 `0700` 的真实临时目录，
不要通过符号链接访问。

### 数据库被拒绝

MiniTun 会拒绝符号链接、多重硬链接、非普通文件、所有权不匹配、错误权限、不安全父
目录、未来模式版本、模式漂移或完整性错误。先停止服务，再检查元数据：

```bash
systemctl stop minitund.service
stat /var/lib/minitun/state.db /var/lib/minitun/credentials.db
namei -l /var/lib/minitun/state.db
```

预期两个数据库都是 `minitun:minitun` 所有的普通文件，权限为 `0600`，硬链接数为
1。不要为了绕过检查而放宽权限，也不要删除数据库尝试“自动修复”。应先备份并根据
具体错误调查模式或文件系统问题。

## CLI 无法连接守护进程

典型表现为退出码 `3` 或无法访问 `/run/minitun/minitun.sock`。

```bash
systemctl is-active minitund.service
stat /run/minitun/minitun.sock
id
getent group minitun
```

套接字应为 `0660` 且属于 `minitun:minitun`。将操作者加入 `minitun` 组后必须重新
登录，现有 Shell 才会获得新组成员身份：

```bash
sudo usermod -aG minitun "$USER"
```

开发构建使用自定义套接字时，每条 CLI 命令都要传相同的 `--socket` 路径。

## 公网服务端无法启动

### TLS 文件缺失或权限错误

默认 unit 需要：

```text
/etc/minitun-server/server.crt
/etc/minitun-server/server.key
/etc/minitun-server/token
```

私钥和 Token 必须是 `minitun-server` 所有的普通文件，且组用户和其他用户没有访问
权限。证书必须与私钥匹配：

```bash
openssl x509 -in /etc/minitun-server/server.crt -noout -subject -issuer -dates
openssl x509 -in /etc/minitun-server/server.crt -pubkey -noout |
  openssl pkey -pubin -outform der | openssl sha256
openssl pkey -in /etc/minitun-server/server.key -pubout -outform der |
  openssl sha256
```

最后两条命令的摘要应一致。不要把私钥内容粘贴到 Issue。

### 监听端口占用

```bash
ss -ltnp | grep ':2333'
ss -ltnp | grep ':6000'
```

修改默认监听地址或端口范围时，请使用 `systemctl edit minitun-server` 创建 override，
不要直接修改发行方 unit。替换 `ExecStart` 前需要先用空的 `ExecStart=` 清除原值。

## 服务器无法认证

检查以下事项：

- `server add` 使用的主机名与证书 SAN 一致；
- `minitund --tls-ca` 指向签发服务端证书的 CA，或系统信任库包含该 CA；
- 客户端输入的 Token 与服务端 Token 文件第一行一致；
- 两台主机时间同步，偏差未超过 `--auth-clock-skew`；
- 防火墙允许访问 TLS 控制端口；
- 服务端认证失败限速没有因连续错误进入短期限制。

查看状态与脱敏日志：

```bash
minitun server list --json
journalctl -u minitund.service -u minitun-server.service --since '-10 min'
```

`--insecure-skip-verify` 只用于隔离开发环境中的临时诊断，不能作为生产修复方案。

## 隧道长期为 `pending`

`pending` 表示隧道期望为活动状态，但尚未在公网服务端完成注册。检查：

1. 对应服务器是否为在线状态；
2. 认证是否成功，Worker 是否能够连接；
3. 公网端口是否位于服务端 `--allow-ports` 范围；
4. 服务端是否达到客户端数、隧道数或连接数上限；
5. 服务端和守护进程是否刚刚重启，状态同步是否仍在进行。

重连采用指数退避，短暂网络故障后不一定立即恢复。不要通过反复重启制造更多连接抖动。

## 隧道状态为 `failed`

稳定错误码可以帮助定位原因：

| 错误码 | 常见原因 | 处理方式 |
| --- | --- | --- |
| `permission_denied` | 公网绑定地址或端口不符合策略 | 检查数值绑定地址与 `--allow-ports` |
| `resource_exhausted` | 达到隧道、Worker 或连接上限 | 检查配置上限和当前资源使用情况 |
| `remote_port_in_use` | 公网端口已被其他进程或隧道占用 | 选择空闲端口或释放现有监听器 |

注册失败只影响对应隧道。修正原因后，后续状态同步会重试；也可以删除并以正确配置
重新创建隧道。

## 公网端口已监听但连接失败

依次确认：

- 隧道实际状态为 `active`；
- 本地目标在客户端主机上可以直接连接；
- `local_host` 没有解析到错误地址；
- 守护进程尚未达到 `--max-total-connections`；
- 服务端没有达到逐客户端或全局连接上限；
- 空闲 Worker 可用，且公网连接没有超过 Worker 等待期限；
- 中间防火墙、云安全组和主机防火墙允许公网隧道端口。

在客户端主机上直接测试本地目标：

```bash
curl -v http://127.0.0.1:8080/
```

根据实际协议替换测试工具。MiniTun 只转发 TCP 字节，不理解 HTTP 或其他应用协议。

## 长连接意外断开

无数据连接会受到 `--relay-idle-timeout` 约束。服务关闭时，活动中继最多排空至
`--shutdown-timeout`。还应检查应用自身超时、负载均衡器、NAT、防火墙和 TCP keepalive
设置。半关闭是正常行为：一个方向 EOF 后，反向数据仍可继续，直至另一方向结束或
超时。

## 软件包安装或卸载问题

DEB/RPM 软件包不会自动启用服务，也不会携带 TLS 材料。安装后需配置凭据再执行：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now minitun-server.service
sudo systemctl enable --now minitund.service
```

普通卸载和升级保留 `/var/lib/minitun` 与 `/var/lib/minitun-server`。Debian 只有
purge 才删除状态目录；RPM 卸载始终保留状态。详细生命周期见[打包指南](packaging.md)。

## 安全地收集诊断信息

提交 Issue 前，建议提供：

```bash
minitun version
minitun status
minitun server list --json
minitun tun list --json
systemctl status minitund.service minitun-server.service --no-pager
```

请删除或替换公网/内网敏感地址、用户标识和资源名称。不要提供以下内容：

- Token 或 Token 文件摘要；
- 私钥、私钥摘要或完整生产证书链；
- `credentials.db`；
- 未脱敏的数据库副本；
- 包含其他用户数据的完整系统日志。
