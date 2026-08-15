# Changelog

> 中文: [CHANGELOG.md](CHANGELOG.md)

All notable changes to MiniTun are recorded in this file. This document is based on the
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) structure, and project versions
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.1] - 2026-08-15

### Added

- P2P paths can carry UDP: `minitun-p2p --udp` forwards local UDP datagrams as
  2-byte length-prefixed records over the direct or relay path to the tunnel's local
  target (negotiated via `MTPV`/`MTFU`, requiring v1.1.1+ on both sides; the legacy TCP
  wire images are unchanged).

## [1.1.0] - 2026-08-15

### Added

- systemd units add `MemoryMax`/`TasksMax` hard resource caps, relaxable per deployment via
  a drop-in.
- Client policy adds the `allowed_source_cidrs` source whitelist and
  `connections_per_minute` per-source connection rate; `minitun-server` adds
  `--max-udp-peer-sessions`.
- Adds a daily state backup systemd timer, OpenRC/s6 supervision recipes, docker-compose
  examples and a Let's Encrypt automatic renewal recipe.
- The P2P direct path upgrades to TLS 1.3 after one-time token authentication, using the
  token as the external PSK to encrypt application data, no longer transmitting in
  plaintext; relay fallback behavior is unchanged.
- tcp tunnels support PROXY protocol v1 headers (`tun add --proxy-protocol`); the server
  carries the public client source endpoint in START_RELAY and the daemon prepends a
  `PROXY TCP4/TCP6` header to the local target, staying byte-compatible with older peers.
- The `minitun-server` management listener gains a `/v1/*` client policy management API:
  list/get, create/update, delete, PSK rotation, and hot reload. Rotation keeps a grace
  window during which both PSKs are accepted and established sessions are not disturbed;
  the new PSK is returned exactly once by the rotation response.
- P2P tunnels gain server-assisted TCP simultaneous open (`tcp_simultaneous_open`
  capability): after a failed direct candidate both sides cross-connect to the other's
  observed endpoint from the same local port, punching NATs with endpoint-independent
  mappings on both ends; `minitun-p2p` adds `--simultaneous-open`/
  `--no-simultaneous-open` (default on; disable it against v1.0 daemons).
- The documentation site adds an English language (Chinese remains the default), plus
  `README.en.md` and `CHANGELOG.en.md`.
- Releases add musl fully static binary archives for `x86_64`/`aarch64` (`static.yml`, no
  glibc/OpenSSL runtime dependency), automatically accompanied by SHA-256.

### Removed

- Removes the old `--token-stdin` CLI alias, keeping only `--psk-stdin`.
- Removes schema v1–v3 (v0.x era) migration support; schema v4 becomes the lowest openable
  version; older databases are rejected and the original files are left unchanged.

## [1.0.0] - 2026-08-13

The first formal release of this generation of source code. All previous v0.x and old
release records were deleted, and the public history restarts from this version.

### Added

- `tcp`, `udp`, `socks5`, `p2p` tunnel modes; non-TCP modes are negotiated via capability,
  keeping the old TCP v2 wire image unchanged.
- Adds UDP tunnel: public UDP peers use bounded sessions/queues, and datagrams are forwarded
  to a fixed local UDP target via 2-byte length records over the authenticated TLS Worker,
  preserving packet boundaries.
- Adds SOCKS5 no-auth CONNECT (IPv4, IPv6, domain); the server bind is forced to a numeric
  loopback to prevent accidental deployment as a public open proxy.
- Adds P2P mode and the `minitun-p2p` connector: a one-time token attempts the direct TCP
  path, automatically falling back to the original TLS relay when unreachable or
  unconfirmed; supports `--relay-only` to verify fallback.
- Per-client PSK, enabled state, public port ACL, tunnel/connection/idle Worker quotas, and
  optional client certificate SAN/SHA-256 binding; policy is validated fully and then
  atomically hot-reloaded.
- A generation-scoped `TunnelReconciler`, server/tunnel `config_revision`, registration
  request ID/revision correlation and a bounded pipeline of at most 32 frames.
- Complete server/tunnel create, update, enable, disable, logout, delete lifecycle, plus
  strict-JSON `config export/plan/apply` and safe `--prune` ownership semantics.
- Stable `libminitun-client.so.1`: C11 opaque ABI, C++20 RAII `Result<T>` wrapper,
  `MiniTun::Client` CMake target, pkg-config, DEB/RPM runtime/devel packages; tunnel
  create/update supports all four modes in a `struct_size`-compatible way.
- Adds `libminitun-remote-protocol.so.1` C++20 SDK: strongly-typed message variant,
  incremental frame decoder, codec and control/Worker authentication digest helpers, with
  CMake/pkg-config integration.
- Adds the disabled-by-default `/healthz`, `/readyz`, `/metrics` admin endpoints to
  daemon/server; non-loopback enforces a Bearer token and metrics use bounded labels.
- Adds policy, authentication, registration/deregistration, ACL/quota and local-management
  audit logging that never records secrets or user traffic.
- Adds schema migration, crash-staged credential cleanup, fault injection, ABI baseline,
  downstream SDK, coverage, clang-tidy, CodeQL, persistent fuzz corpus and standalone
  performance/soak validation.
- The release process adds SPDX/CycloneDX SBOMs, SHA-256, GitHub OIDC
  provenance/attestation, and Sigstore keyless signature verification for executable
  artifacts and OCI.

### Data & protocol

- The state database upgrades to schema v5; historical schema v4 data is migrated
  automatically, preserving stable IDs, names, endpoints, tunnels and original credential
  references, and persisting the four modes and the server bind host.
- Remote Protocol v2 adds `udp_datagrams`, `socks5_proxy`, `p2p_rendezvous` capabilities;
  non-TCP REGISTER/START payloads use a single-byte extension, keeping the original TCP v2
  wire image unchanged.

### Fixed & improved

- Fixes an order-dependent deadlock in tunnel registration tests: window requests are
  coalesced into one TLS application write, avoiding per-frame writes waiting on early
  responses.
- After session interruption, generation change, out-of-order/duplicate/timed-out responses
  or partial writes, residual tunnel state deterministically returns to `pending`; public
  port updates revoke the old listener first, and a failed new bind leaves no old entry.
- TLS session resumption, adaptive Worker capacity, fixed-buffer backpressure and resource
  caps prepare for the formal 100 clients / 2,000 tunnels / 10,000 relay gate.

### Security boundary

- The current P2P does not implement ICE, STUN, TURN or NAT hole punching; the direct path
  upgrades to TLS 1.3 after one-time token authentication (token as external PSK), so
  application data is encrypted end to end.

### Removed

- Removes the `minitun-gui` local web console (C++ HTTP server, React/Vite static assets,
  man page, GUI integration tests and all packaging references), which shipped with an early
  pre-release. The project focuses on a minimal footprint: the control plane is only the CLI
  and the local SDK, with no web GUI.
- Deletes all v0.x and old v1.0.0 releases, tags and migration documents; `v1.0.0` is the
  sole starting point of the project's public history.
