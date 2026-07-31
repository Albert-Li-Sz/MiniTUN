# Development

## Prerequisites

- CMake 3.22 or newer
- Ninja
- A C++20 compiler
- OpenSSL 3 development files
- SQLite3 development files

The `dev` preset uses pinned FetchContent releases for CLI11, standalone Asio,
nlohmann/json, spdlog, and GoogleTest. The `release` preset uses system packages.

## Configure, build, and test

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Useful configuration switches include:

```text
MINITUN_USE_SYSTEM_DEPS
MINITUN_BUILD_TESTS
MINITUN_BUILD_FUZZERS
MINITUN_ENABLE_ASAN
MINITUN_ENABLE_UBSAN
MINITUN_ENABLE_TSAN
MINITUN_ENABLE_LTO
MINITUN_BUILD_PACKAGES
```

Sanitizer builds have their own presets. TSan is intentionally kept separate from
ASan/UBSan.

## Stage-3 IPC development

The IPC wire format is a four-byte network-order payload length followed by UTF-8 JSON.
Both directions are limited to 1 MiB. The public protocol layer performs strict schema
validation before a request reaches a method handler, and the Unix-domain-socket layer
handles concurrent single-request sessions without exposing SQLite to the CLI. Handler
execution is bounded to four worker threads and remains covered by each session's
absolute deadline. Pool shutdown rejects new submissions before joining accepted work.

Exercise a real daemon-status round trip in a private directory:

```bash
runtime_dir="$(mktemp -d)"
build/dev/minitund --socket "$runtime_dir/minitun.sock"
# In another terminal:
build/dev/minitun --socket "$runtime_dir/minitun.sock" daemon status
```

Run only IPC-focused tests with:

```bash
ctest --test-dir build/dev --output-on-failure -R '(Ipc|Frame|Dispatcher|cli-daemon)'
```

The IPC tests use isolated, physically resolved temporary socket paths. Decoder tests
cover partial and coalesced frames; transport tests cover malformed-client isolation,
deadlines, concurrent requests, single-request connections, pool shutdown, permissions,
trusted path ancestry, serialized startup, and cleanup.

## Persistence development

The storage implementation uses the system SQLite3 library in both dependency modes.
Its default production path is `/var/lib/minitun/state.db`, but unit tests create
isolated temporary database files and do not touch that location.

Run only the storage and recovery tests after a persistence change with:

```bash
ctest --test-dir build/dev --output-on-failure -R '(Storage|Recovery)'
```

The tests cover fresh and repeated migration, refusal of future, drifted, or malformed
schemas, migration rollback, connection policy, transaction commit/rollback/isolation,
repository validation and constraints, monotonic timestamps, tombstone behavior,
restart-state recovery, and reopen persistence.

`minitund` links `MiniTun::storage` and now runs the stage-3 IPC service, but it does not
yet accept `--database`, open the default database, or run recovery from `main()`.
Those lifecycle integrations belong to later daemon-runtime stages. Until then,
exercise persistence through repository unit tests rather than treating the
executables as a working tunnel service.
