# MiniTun

MiniTun is an independently implemented TCP reverse-tunnelling system for Linux. It
uses C++20 and is intentionally not compatible with the FRP protocol.

The repository is currently at **development stage 7**. The local control plane is
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

Authentication material is stored separately in `/var/lib/minitun/credentials.db`,
whose file mode is enforced as `0600`; Tokens are never stored in `state.db`, returned
by IPC, or printed by the CLI. `server login` stores a Token and wakes reconciliation;
the daemon then authenticates the corresponding remote session without exposing the
secret.

This is not yet a deployable tunnel service. Tunnel registration, worker pools, TCP
relay, service installation, and packages belong to later stages.

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
  --token-file /path/to/token
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
`/var/lib/minitun/state.db`, and `/var/lib/minitun/credentials.db`. Packaging will
create the `minitun:minitun` account and protected runtime/state directories in a later
stage; the program deliberately does not create those top-level directories.

See [CLI reference](docs/cli.md), [development notes](docs/development.md), and the
[architecture overview](docs/architecture.md).

## License

MiniTun is available under the MIT License. See [LICENSE](LICENSE).
