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
- `minitun`, `minitund`, and `minitun-server` stage-0 executables.
- Unit-test and GitHub Actions CI foundations.
