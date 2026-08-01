# MiniTun

MiniTun is an independently implemented TCP reverse-tunnelling system for Linux. It
uses C++20 and is intentionally not compatible with the FRP protocol.

The repository is currently at **development stage 13**. The local control plane is
operational: the stateless `minitun` CLI talks to `minitund` over a protected Unix
socket, and the daemon persists server and tunnel intent in SQLite. It supports all
`server`, `tun`, `status`, and `daemon status` commands, structured JSON inspection,
restart recovery, concurrent CLI requests, stable exit codes, and non-echoing Token
input. The remote protocol library now provides bounded network-byte-order frames,
incremental decoding, binary payload fields, strict message types, control/worker
state validation, and a libFuzzer harness. `minitun-server` now exposes a real TLS
control listener with challenge-response authentication, session generations,
heartbeat timeouts, replay protection, and authentication rate limiting. `minitund`
now persists a stable client identity and runs one strand-isolated TLS control session,
heartbeat, and exponential reconnect controller per configured server. A failed or
restarting server does not interrupt the daemon's other online server sessions.
Active tunnel intent is now reconciled over those sessions into allowlisted public TCP
listeners. Registration failures remain isolated to one tunnel, and listeners are
restored after either endpoint restarts and released after tunnel removal. Each remote
server now has an isolated TLS Worker Pool with generation checks, bounded per-session
and global capacity, automatic replenishment, two-second public-connection waits, and
idle Worker reclamation.
Assigned Workers now resolve the locally persisted target, establish it asynchronously,
and switch to a raw TLS byte stream. The relay uses fixed 16 KiB buffers in each
direction, read/write backpressure, TCP half-close propagation, cancellation,
inactivity deadlines, and byte statistics.

Authentication material is stored separately in `/var/lib/minitun/credentials.db`.
Both SQLite databases enforce daemon ownership, exact `0600` permissions, one hard
link, a trusted private parent directory, no symbolic-link traversal, and stable inode
identity across open. Tokens are never stored in `state.db`, returned by IPC, or
printed by the CLI. `server login` stores a Token and wakes reconciliation; the daemon
then authenticates the corresponding remote session without exposing the secret.

The TCP data path and graceful lifecycle are operational. ASan, UBSan, TSan, and five
libFuzzer entry points cover the security-sensitive parsers and concurrent shutdown
paths. Component-aware CMake installation now provides hardened systemd services,
systemd-sysusers definitions, man pages, and the documented `/usr` Linux layout.
Remaining work focuses on DEB/RPM packages and release automation.

## Run the TLS server

`minitun-server` requires a PEM certificate, matching private key, and a Token file
owned by the service user with no group or other permissions:

```bash
chmod 0600 /path/to/token
build/dev/minitun-server \
  --foreground \
  --listen 0.0.0.0:2333 \
  --tls-cert /path/to/server.crt \
  --tls-key /path/to/server.key \
  --token-file /path/to/token \
  --allow-ports 6000-6999
```

## Build

Ninja, CMake 3.22 or newer, a C++20 compiler, OpenSSL 3, and SQLite3 are required. The
developer preset downloads pinned releases of the remaining dependencies.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Use distribution-provided dependencies for release builds:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## Try the local control plane

Use a physically resolved, private directory during development:

```bash
runtime_root="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
runtime_dir="$(mktemp -d "$runtime_root/minitun.XXXXXX")"

build/dev/minitund \
  --socket "$runtime_dir/minitun.sock" \
  --database "$runtime_dir/state.db" \
  --credentials "$runtime_dir/credentials.db" \
  --tls-ca /path/to/server-ca.crt
```

In another terminal:

```bash
build/dev/minitun --socket "$runtime_dir/minitun.sock" \
  server add tunnel.example.com:2333 --name primary

printf '%s\n' "$MINITUN_TOKEN" |
  build/dev/minitun --socket "$runtime_dir/minitun.sock" \
    server login primary --token-stdin

build/dev/minitun --socket "$runtime_dir/minitun.sock" \
  tun add primary 22 6000 --name ssh
build/dev/minitun --socket "$runtime_dir/minitun.sock" server list
build/dev/minitun --socket "$runtime_dir/minitun.sock" tun list --json
build/dev/minitun --socket "$runtime_dir/minitun.sock" status
```

The production paths are `/run/minitun/minitun.sock`,
`/var/lib/minitun/state.db`, and `/var/lib/minitun/credentials.db`. The installed
sysusers definition creates the `minitun:minitun` account, and systemd creates protected
runtime/state directories. The program deliberately does not create those top-level
directories itself.

See [installation](docs/installation.md), [CLI reference](docs/cli.md),
[development notes](docs/development.md), and the [architecture overview](docs/architecture.md).

## License

MiniTun is available under the MIT License. See [LICENSE](LICENSE).
