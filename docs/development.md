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

## Stage-2 persistence development

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

`minitund` currently links `MiniTun::storage` but remains a stage-2 placeholder. It
does not yet accept `--database`, open the default database, or run recovery from
`main()`. Those user-facing and lifecycle integrations belong to the IPC and daemon
runtime stages. Until then, exercise persistence through the repository unit tests
rather than treating the executables as a working tunnel service.
