# Installation Guide

This page explains how to obtain, verify and install MiniTun. The project focuses on a
minimal footprint: just five executables (`minitun`, `minitund`, `minitun-server`,
`minitun-p2p`) and two SDK shared libraries, with no web GUI and no scripting runtime.
Linux/systemd is the officially supported target.

## Release matrix

| Format | Runtime | Architectures |
| --- | --- | --- |
| DEB | Debian/Ubuntu | `amd64`, `arm64`, `armhf`, `riscv64` |
| RPM | Fedora/RHEL family | `x86_64`, `aarch64`, `armv7hl`, `riscv64` |
| OCI | Docker / containerd | `linux/amd64`, `linux/arm64`, `linux/arm/v7`, `linux/riscv64` |
| static tar | no runtime dependencies | fully static musl binaries for `x86_64`, `aarch64` |

Each release contains four packages:

| Package | Contents |
| --- | --- |
| `minitun-server` | public server, systemd unit, `minitun-server` service account |
| `minitun-client` | CLI `minitun`, daemon `minitund`, P2P connector `minitun-p2p`, systemd unit, `minitun` service account |
| `libminitun-client1` | runtime library for the local control SDK and the Remote Protocol SDK |
| `libminitun-client-dev` / `libminitun-client-devel` | development headers, CMake targets and pkg-config metadata (optional) |

## 1. Download and verify

Download the four packages for the target version and architecture from
[GitHub Releases](https://github.com/Albert-Li-Sz/MiniTUN/releases), along with
`SHA256SUMS` and the corresponding `.sigstore.json` bundle. Verify the digests first:

```bash
cd /path/to/downloads
sha256sum --check SHA256SUMS
```

Then verify the keyless signature with `cosign` (see
[sigstore/cosign](https://docs.sigstore.dev/cosign/installation/) for installation):

```bash
cosign verify-blob \
  --bundle=minitun-server_<version>_amd64.deb.sigstore.json \
  --certificate-identity="https://github.com/Albert-Li-Sz/MiniTUN/.github/workflows/release.yml@refs/tags/<version-tag>" \
  --certificate-oidc-issuer=https://token.actions.githubusercontent.com \
  minitun-server_<version>_amd64.deb
```

The release page also provides SPDX/CycloneDX SBOMs and provenance attestation. A failed
verification means the transfer is corrupted or tampered with; do not continue installing.

## 2. DEB (Debian/Ubuntu)

```bash
sudo apt install ./minitun-server_<version>_amd64.deb
sudo apt install ./minitun-client_<version>_amd64.deb
# development SDK (optional)
sudo apt install ./libminitun-client1_<version>_amd64.deb \
  ./libminitun-client-dev_<version>_amd64.deb
```

The install process will:

- create the dedicated `minitun-server` and `minitun` service accounts via
  systemd-sysusers;
- install and `daemon-reload` `minitund.service` and `minitun-server.service`;
- **not** generate any credentials and **not** auto-start the services.

Verify the installation:

```bash
minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
minitun-p2p --version
systemctl status minitund.service minitun-server.service
```

## 3. RPM (Fedora/RHEL)

```bash
sudo dnf install ./minitun-server-<version>.x86_64.rpm
sudo dnf install ./minitun-client-<version>.x86_64.rpm
# development SDK (optional)
sudo dnf install ./libminitun-client1-<version>.x86_64.rpm \
  ./libminitun-client-devel-<version>.x86_64.rpm
```

Behavior is identical to DEB: it creates service accounts, installs systemd units,
generates no credentials and does not auto-start. Architecture names map to the distro:
`x86_64`, `aarch64`, `armv7hl`, `riscv64`.

## 4. OCI images

Images are based on `debian:stable-slim` and run as non-root (UID 65532), with no build
tools:

- `ghcr.io/albert-li-sz/minitun-server`: the server;
- `ghcr.io/albert-li-sz/minitun-client`: the client daemon (includes the CLI and system CA).

Tags match the release version (e.g. `:1.0.0`). The server needs the certificate, private
key, client policies and PSK directory mounted:

```bash
sudo mkdir -p /etc/minitun-server
# put server.crt, server.key, clients.json and each client PSK in that directory, then:
docker run -d --name minitun-server \
  --network host \
  -v /etc/minitun-server:/etc/minitun-server:ro \
  ghcr.io/albert-li-sz/minitun-server:1.0.0
```

The client daemon needs `/var/lib/minitun` (state and credentials) and `/run/minitun` (IPC
socket) persisted:

```bash
docker run -d --name minitund \
  --network host \
  -v /var/lib/minitun:/var/lib/minitun \
  -v /run/minitun:/run/minitun \
  ghcr.io/albert-li-sz/minitun-client:1.0.0
```

Inside the container, use the same CLI via `docker exec minitund minitun daemon status`.
Images run the binary directly with `--foreground`; exit codes and log semantics match a
bare-metal deployment.

## 5. Building from source

You need CMake 3.22+, Ninja, a C++20-capable GCC/Clang, OpenSSL 3, SQLite3 and Python 3.
The `dev` preset fetches the remaining dependencies at pinned versions:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Artifacts are under `build/dev/`. Install in stages as root (Client component only):

```bash
cmake --install build/dev --prefix /usr --component Client
```

For production, generate distribution packages from source (Linux, requires
`MINITUN_BUILD_PACKAGES`):

```bash
cmake --preset package-deb   # Debian/Ubuntu
cmake --build --preset package-deb --parallel
cpack --config build/package-deb/CPackConfig.cmake -G DEB
```

See the [Development Guide](/en/development) for details.

## 6. First deployment

### 6.1 Obtain the client identity and generate a PSK

On the internal host where `minitund` runs:

```bash
sudo systemctl enable --now minitund.service
minitun daemon identity --json
```

The `client_id` in the output is the stable identity. Generate a dedicated PSK for it
(**never** readable by group or other users):

```bash
umask 077
openssl rand -hex 32 > team-a.psk
```

### 6.2 Configure the public server

After installing `minitun-server` on the public server, place the certificate and policy
files (all owned by the `minitun-server` service account):

```bash
sudo install -d -m 0750 -o minitun-server -g minitun-server \
  /etc/minitun-server/clients
sudo install -m 0600 -o minitun-server -g minitun-server team-a.psk \
  /etc/minitun-server/clients/team-a.psk
sudo install -m 0640 -o minitun-server -g minitun-server clients.json \
  /etc/minitun-server/clients.json
sudo install -m 0644 server.crt /etc/minitun-server/server.crt
sudo install -m 0600 -o minitun-server -g minitun-server server.key \
  /etc/minitun-server/server.key
```

Minimal policy (`clients.json`):

```json
{
  "format_version": 1,
  "clients": [
    {
      "client_id": "client_0123456789abcdef0123456789abcdef",
      "enabled": true,
      "psk_file": "/etc/minitun-server/clients/team-a.psk",
      "allowed_ports": ["6000-6099"],
      "max_tunnels": 100,
      "max_connections": 1000,
      "max_idle_workers": 32
    }
  ]
}
```

Start and check:

```bash
sudo systemctl enable --now minitun-server.service
systemctl status minitun-server.service
```

The default control port is `2333/tcp`. The cloud security group and host firewall must
also only allow the public tunnel ports that the policy permits and that are actually
used. See the [Configuration documentation](/en/configuration) for the complete fields.

### 6.3 Connect the daemon and create a tunnel

Back on the internal host, forward public port `6000` to local `127.0.0.1:8080`:

```bash
minitun server add tunnel.example.com:2333 --name edge
minitun server login edge --psk-stdin < /secure/path/team-a.psk
minitun tun add edge 8080 6000 --name web
minitun tun inspect web --json
```

Once the tunnel's `actual_state` is `active`, public `tunnel.example.com:6000` forwards to
internal `127.0.0.1:8080`. Synchronization is asynchronous; if it stays `pending` or
`failed`, check `server_actual_state`, `pending_reason`, `last_error` and the audit logs on
both sides.

Creating UDP, SOCKS5 and P2P modes:

```bash
# UDP: public 6001/udp -> internal 127.0.0.1:5353/udp
minitun tun add edge 5353 6001 --protocol udp --name dns-udp

# SOCKS5: public 6002/tcp provides CONNECT; local-port is a CLI-compatible placeholder
minitun tun add edge 1 6002 --protocol socks5 \
  --remote-host 127.0.0.1 --name private-proxy

# P2P: create the entry first, then run the connector on the access side
minitun tun add edge 8080 6003 --protocol p2p --name p2p-web
minitun-p2p tunnel.example.com:6003 --listen 127.0.0.1:6501
```

The SOCKS5 `--remote-host` must be a numeric loopback to prevent accidentally deploying an
open public proxy; the P2P connector listens on loopback only by default. See the
[CLI documentation](/en/cli) for the full command set.

## 7. Upgrade and uninstall

### Upgrading

Install over the same distribution format directly; ordinary upgrades and uninstalls keep
the state directories:

- Debian: `sudo apt install ./minitun-client_<new-version>_amd64.deb ...`
- Fedora: `sudo dnf upgrade ./minitun-client-<new-version>.x86_64.rpm ...`

Packages **do not** overwrite administrator-provided certificates, private keys, PSKs or
client policies.

### Uninstalling

```bash
# Debian/Ubuntu: keep state directories; purge removes them
sudo apt remove minitun-client minitun-server
sudo apt purge minitun-client minitun-server   # removes /var/lib/minitun and /var/lib/minitun-server

# Fedora/RHEL: state directories are kept after uninstall and must be cleaned up manually
sudo dnf remove minitun-client minitun-server
```

Uninstalling does not remove the certificates and policies under `/etc/minitun-server`;
remove them manually when they are no longer needed.

## 8. Troubleshooting

Check in order:

```bash
minitun version                     # client version and build info
systemctl status minitund.service minitun-server.service
journalctl -u minitund -u minitun-server -n 200 --no-pager
minitun daemon status --json        # daemon and each server session state
minitun doctor --json --checkpoint  # SQLite diagnostics, WAL checkpoint and online backup
```

Common issues:

- **Tunnel stays `pending`**: check `server_actual_state`, `pending_reason` and
  `last_error`; common causes are not logged in, PSK mismatch, the public port not being
  in `allowed_ports`, or the policy not being reloaded.
- **Server policy reload**: after editing `clients.json`, run
  `sudo systemctl reload minitun-server`; an invalid new config keeps the current snapshot.
- **Port in use**: public port conflicts map to an explicit error; free the port and run
  `minitun tun enable` to recover.
- **Admin endpoints**: when you need `/healthz`, `/readyz` or `/metrics`, add
  `--admin-listen 127.0.0.1:<port>` to the daemon/server, as described in the
  [Operations documentation](/en/operations).
