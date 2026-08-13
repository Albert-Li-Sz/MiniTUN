# 运维与可观测性

## 管理端点

`minitund` 与 `minitun-server` 都提供默认关闭的 HTTP 管理端点：

```text
GET  /healthz
HEAD /healthz
GET  /readyz
HEAD /readyz
GET  /metrics
```

回环监听可以不认证：

```bash
minitund --admin-listen 127.0.0.1:9091 ...
minitun-server --admin-listen 127.0.0.1:9090 ...
```

非回环监听必须同时配置私有 `--admin-token-file`，请求使用
`Authorization: Bearer <token>`。管理端点本身不提供 TLS；非回环模式只能用于可信管理
网络，或放在启用 TLS 和访问控制的反向代理之后。

HTTP 实现只接受已列出的方法和路径，并限制 header/body 大小、并发连接数和超时。
`HEAD` 返回与对应 `GET` 相同的状态和 header，但不返回 body。

健康和就绪含义不同：

- daemon 健康检查状态库与凭据库；就绪还要求 IPC 已正常监听，但不要求所有远端在线；
- server 健康检查进程，server 就绪要求 TLS、客户端策略和控制 listener 均可用。

## 指标

`/metrics` 输出 Prometheus 文本，覆盖 session、连接、隧道、Worker、relay、认证、注册、
ACL/配额拒绝、错误、重连、字节、TLS 会话恢复、策略重载、队列和注册延迟。标签只使用
`role`、`state`、`result`、`direction` 等固定集合，不使用 client/tunnel ID 或名称。

所有计数器都在进程重启后归零。持久化业务状态应从本地 IPC/SDK 查询，不能用指标
计数器代替。

## 审计日志

以下事件使用 `server.audit` 或 `daemon.audit` 组件记录：

- 策略重载及其结果；
- 认证成功与通用失败；
- 隧道注册、注销和管理操作；
- ACL、连接、隧道和 Worker 配额拒绝；
- 凭据 logout、enable/disable、update、apply 和 prune。

日志不包含 PSK、证书内容、私钥、认证摘要或用户流量。客户端和隧道标识只出现在单条
结构化审计事件中，不进入指标标签。

## 常用探测

```bash
curl --fail http://127.0.0.1:9090/healthz
curl --fail http://127.0.0.1:9090/readyz
curl --fail http://127.0.0.1:9090/metrics

minitun health
minitun readiness
minitun metrics
minitun doctor --json --checkpoint
```

服务端策略和 TLS 材料由 `SIGHUP` 重载；daemon 的 `SIGHUP` 或 `minitun reload` 会重建
远程 session。更新单个 server 的 endpoint、TLS server name 或凭据时，只重启该 server
的 session，其他 server 和已建立 relay 不受影响。

## 备份与恢复

生产升级前同时备份状态与凭据数据库：

```bash
install -d -m 0700 /var/backups/minitun/pre-v1
minitun doctor --json \
  --backup-state /var/backups/minitun/pre-v1/state.db \
  --backup-credentials /var/backups/minitun/pre-v1/credentials.db
```

备份是成对的；恢复时也应使用同一时间点生成的两个文件。

