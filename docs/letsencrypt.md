# Let's Encrypt 证书配方

MiniTun 的 server 需要一对 TLS 证书。用 Let's Encrypt 自动签发并续期时，
`minitun-server` 通过 SIGHUP 热加载新证书，续期无需重启服务。

## 1. 用 certbot 签发

```bash
sudo apt install certbot        # Debian/Ubuntu；Fedora 用 dnf install certbot
sudo certbot certonly --standalone -d tunnel.example.com \
  --deploy-hook 'install -m 0644 "$RENEWED_LINEAGE/fullchain.pem" /etc/minitun-server/server.crt && \
                 install -m 0600 -o minitun-server -g minitun-server "$RENEWED_LINEAGE/privkey.pem" /etc/minitun-server/server.key && \
                 systemctl reload minitun-server'
```

说明：

- `--standalone` 需要 80/tcp 空闲（certbot 临时监听）；已有 Web 服务时改用
  `--webroot` 或 DNS 验证插件；
- `--deploy-hook` 在每次成功续期后执行，把证书链与私钥复制到
  `/etc/minitun-server/` 并触发 `systemctl reload`；
- `reload` 是原子热加载：无效新配置/证书会保留当前快照，正在传输的 relay
  不受影响。

## 2. 首次签发后的收尾

certbot 的 systemd timer（`certbot.timer`）会自动续期。首次手动签发完成后，
直接启动或重载 server：

```bash
sudo systemctl enable --now minitun-server.service
sudo systemctl reload minitun-server.service
```

## 3. 验证

```bash
openssl x509 -in /etc/minitun-server/server.crt -noout -dates -subject
sudo certbot certificates
```

> 私有 CA 场景（内网设备自签）不需要本配方：按[安装指南](/installation)把
> CA 链复制到 `/etc/minitun-server/server.crt` 即可，客户端用 `--tls-ca`
> 信任该 CA。
