# 从 v0.4.1 升级到 v1.0

v1 的 Remote Protocol v2 与 v0.4.x 不兼容，升级必须安排 server/client 协调停机。
本地 IPC envelope 仍为 v1，但状态数据库会自动迁移到 schema v4；旧程序不能打开
已经迁移的数据库。

## 升级前检查

1. 确认当前运行版本为 v0.4.1，状态和凭据诊断均健康。
2. 导出资源清单并记录所有公网端口、防火墙和 systemd override。
3. 使用在线备份生成同一时间点的 `state.db` 与 `credentials.db` 副本。
4. 为每个 daemon 记录 `minitun daemon identity --json` 返回的稳定 `client_id`。
5. 为每个 client 创建独立 PSK，并生成严格的 `clients.json`；不要复用旧的全局 Token。
6. 如启用客户端证书绑定，先验证 CA、证书链、SAN/指纹和私钥权限。

```bash
minitun doctor --json --checkpoint
minitun config export >pre-v1-resources.json
install -d -m 0700 /var/backups/minitun/pre-v1
minitun doctor --json \
  --backup-state /var/backups/minitun/pre-v1/state.db \
  --backup-credentials /var/backups/minitun/pre-v1/credentials.db
```

## 升级顺序

1. 停止所有 `minitund`，再停止 `minitun-server`。
2. 安装 v1 server、client、SDK runtime/devel 软件包。
3. 用 `--clients-config` 替代旧 `--token-file`，按需增加 `--client-ca`。
4. 启动 server，确认 `/readyz` 成功。
5. 启动每个 daemon。首次打开会在单个事务中执行 v3→v4 迁移，保留 server/tunnel ID、
   名称、端点、状态和旧凭据引用，并增加 revision、TLS 凭据引用和 ownership 字段。
6. 为每个 server 更新 PSK/TLS 材料，再检查 tunnel 收敛。

```bash
sudo systemctl stop minitund.service minitun-server.service
# 安装 v1 软件包并配置 clients.json
sudo systemctl start minitun-server.service
curl --fail http://127.0.0.1:9090/readyz
sudo systemctl start minitund.service
minitun doctor --json
minitun status --json
```

迁移测试覆盖真实 schema v3（v0.4.1）数据库，断言稳定 ID、凭据引用、server 名称和
tunnel 本地/远程端点不变。

## 回滚

迁移完成后，v0.4.1 会拒绝 schema v4。回滚不能只降级二进制，必须：

1. 停止 v1 daemon/server；
2. 还原升级前成对备份；
3. 恢复 v0.4.1 的 server 配置和协调停机时使用的全局凭据；
4. 安装 v0.4.1 并按 server→daemon 顺序启动。

```bash
sudo systemctl stop minitund.service minitun-server.service
# 服务保持停止；先保存当前 v1 文件用于故障分析，再离线恢复成对备份。
sudo install -m 0600 -o minitun -g minitun \
  /var/backups/minitun/pre-v1/state.db /var/lib/minitun/state.db
sudo install -m 0600 -o minitun -g minitun \
  /var/backups/minitun/pre-v1/credentials.db /var/lib/minitun/credentials.db
# 删除仅属于已停止 v1 数据库的旁车文件，然后安装/启动 v0.4.1。
sudo rm -f /var/lib/minitun/state.db-wal /var/lib/minitun/state.db-shm \
  /var/lib/minitun/credentials.db-wal /var/lib/minitun/credentials.db-shm
```

不得在 daemon 运行时直接复制 SQLite 主文件。上述路径是软件包默认值；执行离线覆盖前
先核对实际 systemd 参数和备份来源，并保留当前 v1 文件。恢复完成后先验证文件 owner/
mode，再按 server→daemon 顺序启动 v0.4.1。
