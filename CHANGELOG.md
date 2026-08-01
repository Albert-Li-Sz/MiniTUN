# Changelog

All notable changes to MiniTun will be documented here.

## [Unreleased]

### Added

- C++20 CMake and Ninja project foundation.
- System-dependency and pinned FetchContent dependency modes.
- Common error, result, structured logging, and build-version modules.
- Strict domain, IPv4, bracketed-IPv6 endpoint parsing and TCP port ranges.
- Typed 128-bit random IDs backed by OpenSSL's CSPRNG.
- Overflow-safe Unix/monotonic time helpers and thread-safe UTC formatting.
- Move-only secret storage with explicit OpenSSL memory cleansing.
- Bounded structured-log fields with UTF-8-safe truncation.
- Transactional SQLite schema-version migration with WAL, foreign-key, synchronous,
  busy-timeout, checkpoint, and journal-size policy verification.
- Validated `ServerRepository` and `TunnelRepository` CRUD, tombstones, storage limits,
  deterministic queries, and cross-repository transactions.
- Atomic restart-state normalization and recovery snapshots for persisted servers and
  tunnels.
- Strict 1 MiB length-prefixed UTF-8 JSON IPC request and response codecs.
- Thread-safe method dispatch with stable error responses and exception containment.
- Concurrent Unix-domain-socket client/server transport with deadlines, connection
  limits, exact `0660` permissions, trusted-path validation, serialized startup,
  and safe stale-socket cleanup.
- Real `minitun daemon status` communication with the local `minitund` process.
- Complete `server`, `tun`, and aggregate `status` CLI commands with JSON list
  and inspect output plus stable `0/2/3/4/5/10` exit codes.
- Daemon control service backed by consistent SQLite transactions, restart recovery,
  tombstone filtering, and concurrent local request handling.
- Separate SQLite credential store with a `0600` file, transactional put/get/remove,
  schema checks, secure deletion, and opaque references from `state.db`.
- Non-echoing interactive Token input, explicit `--token-stdin`, and cleansing of
  Token-bearing CLI and IPC buffers.
- Versioned 24-byte remote binary frames with explicit network-byte-order encoding,
  a strict 64 KiB bound, incremental stream decoding, and complete message types.
- Bounded binary payload primitives, strict UTF-8 fields, control/worker connection
  state validation, and an ASan/UBSan-capable libFuzzer remote-frame target.
- TLS 1.2-or-newer server transport with certificate/key validation, explicit framing,
  bounded handshake and heartbeat deadlines, and fixed Asio I/O threads.
- HELLO/AUTH message codecs, HMAC-SHA256 challenge authentication, constant-time
  digest verification, bounded nonce replay cache, per-address failure throttling,
  isolated session generations, and generic authentication failures.
- Runtime-generated-certificate integration coverage for trusted and untrusted TLS,
  correct and incorrect Tokens, heartbeat exchange, and Token-safe server logs.
- Schema-version-2 migration with a transactionally generated, restart-stable daemon
  client identity.
- Daemon-side multi-server manager with isolated TLS control sessions, authentication,
  heartbeat state, session generations, and jittered per-server reconnect backoff.
- `minitund` CA selection, fixed I/O thread count, structured log-level selection, and
  an explicitly warned development-only TLS verification bypass.
- Dual-server integration coverage for failure isolation, server restart recovery,
  daemon restart recovery, and stable identity reuse.
- Bounded REGISTER/UNREGISTER tunnel payload codecs with correlated success and stable
  failure responses.
- Server-side public listener registry with numeric bind validation, explicit port
  allowlists, per-client tunnel limits, idempotent removal, and port-conflict mapping.
- Daemon tunnel reconciliation that persists `registering`, `active`, `failed`,
  `pending`, and `removing` transitions and restores listeners after reconnect.
- Registration integration coverage for policy rejection, port conflicts and recovery,
  listener release, and daemon/server restart restoration.
- Isolated, generation-scoped Worker Pools with bounded per-server and global idle
  capacity, automatic replenishment, public-connection wait deadlines, and idle expiry.
- A bidirectional TCP relay with fixed 16 KiB buffers, read/write backpressure,
  half-close propagation, inactivity deadlines, cancellation, and byte statistics.
- Graceful signal handling with best-effort `GOAWAY`, deadline-bounded relay draining,
  per-client and global connection quotas, and restart/recovery integration coverage.
- Component-aware Linux installation with hardened systemd units, systemd-sysusers
  definitions, man pages, public headers, and staged layout verification.
- Separate `minitun-client` and `minitun-server` DEB/RPM packages with service-account
  creation, daemon reload hooks, state-preserving upgrades/removals, DEB purge cleanup,
  package-content inspection, and clean-container smoke tests.
- GitHub Actions compiler, sanitizer, bounded-fuzz, DEB/RPM, and tag-release workflows
  with reusable tested packaging, versioned artifacts, SHA-256 manifests, minimal token
  permissions, concurrency cancellation, and grouped Dependabot updates.
- A stage-16 final acceptance record covering tests, sanitizers, E2E behavior, packages,
  documentation, and release readiness.
- `minitun`, `minitund`, and `minitun-server` executables.
- Unit-test and GitHub Actions CI foundations.

### Fixed

- GCC 11/12 coroutine frame lifetime handling for moved protocol payloads and detached
  TLS GOAWAY writes.
