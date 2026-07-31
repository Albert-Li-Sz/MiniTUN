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
- Complete stage-4 `server`, `tun`, and aggregate `status` CLI commands with JSON list
  and inspect output plus stable `0/2/3/4/5/10` exit codes.
- Daemon control service backed by consistent SQLite transactions, restart recovery,
  tombstone filtering, and concurrent local request handling.
- Separate SQLite credential store with a `0600` file, transactional put/get/remove,
  schema checks, secure deletion, and opaque references from `state.db`.
- Non-echoing interactive Token input, explicit `--token-stdin`, and cleansing of
  Token-bearing CLI and IPC buffers.
- `minitun`, `minitund`, and `minitun-server` stage-0 executables.
- Unit-test and GitHub Actions CI foundations.
