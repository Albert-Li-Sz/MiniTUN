# MiniTun

MiniTun is an independently implemented TCP reverse-tunnelling system for Linux. It
uses C++20 and is intentionally not compatible with the FRP protocol.

The repository is currently at **development stage 5**. The local control plane is
operational: the stateless `minitun` CLI talks to `minitund` over a protected Unix
socket, and the daemon persists server and tunnel intent in SQLite. It supports all
`server`, `tun`, `status`, and `daemon status` commands, structured JSON inspection,
restart recovery, concurrent CLI requests, stable exit codes, and non-echoing Token
input. The remote protocol library now provides bounded network-byte-order frames,
incremental decoding, binary payload fields, strict message types, control/worker
state validation, and a libFuzzer harness.

Authentication material is stored separately in `/var/lib/minitun/credentials.db`,
whose file mode is enforced as `0600`; Tokens are never stored in `state.db`, returned
by IPC, or printed by the CLI. `server login` currently stores a Token and changes the
local server state to `disconnected`. It does not contact or authenticate with the
public server yet.

This is not yet a deployable tunnel service. TLS and remote authentication,
multi-server sessions, tunnel registration, worker pools, TCP relay, service
installation, and packages belong to later stages.

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
  --credentials "$runtime_dir/credentials.db"
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
