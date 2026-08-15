# Let's Encrypt Certificate Recipes

The MiniTun server needs a TLS certificate pair. When issuing and renewing automatically
with Let's Encrypt, `minitun-server` hot-reloads the new certificate via SIGHUP, so
renewal never requires restarting the service.

## 1. Issuing with certbot

```bash
sudo apt install certbot        # Debian/Ubuntu; use dnf install certbot on Fedora
sudo certbot certonly --standalone -d tunnel.example.com \
  --deploy-hook 'install -m 0644 "$RENEWED_LINEAGE/fullchain.pem" /etc/minitun-server/server.crt && \
                 install -m 0600 -o minitun-server -g minitun-server "$RENEWED_LINEAGE/privkey.pem" /etc/minitun-server/server.key && \
                 systemctl reload minitun-server'
```

Notes:

- `--standalone` needs 80/tcp free (certbot listens temporarily); use `--webroot` or a DNS
  validation plugin when a web server is already running;
- `--deploy-hook` runs after every successful renewal, copying the certificate chain and
  private key into `/etc/minitun-server/` and triggering `systemctl reload`;
- `reload` is an atomic hot reload: an invalid new config/certificate keeps the current
  snapshot, and in-flight relays are unaffected.

## 2. Cleanup after the first issuance

certbot's systemd timer (`certbot.timer`) renews automatically. After the first manual
issuance, start or reload the server directly:

```bash
sudo systemctl enable --now minitun-server.service
sudo systemctl reload minitun-server.service
```

## 3. Verification

```bash
openssl x509 -in /etc/minitun-server/server.crt -noout -dates -subject
sudo certbot certificates
```

> A private-CA scenario (self-signed internal devices) does not need this recipe: copy the
> CA chain to `/etc/minitun-server/server.crt` as described in the
> [Installation Guide](/en/installation), and have clients trust that CA with `--tls-ca`.
