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
ASan/UBSan:

```bash
cmake --preset asan
cmake --build --preset asan --parallel 2
ctest --preset asan

cmake --preset ubsan
cmake --build --preset ubsan --parallel 2
ctest --preset ubsan

cmake --preset tsan
cmake --build --preset tsan --parallel 2
ctest --preset tsan
```

Build and smoke-test every fuzz target with Clang and libFuzzer:

```bash
cmake --preset fuzz
cmake --build --preset fuzz --parallel 2
for target in remote_frame ipc_frame ipc_json endpoint port_range; do
  "build/fuzz/minitun_${target}_fuzz" -runs=2000 -max_total_time=10
done
```

Apple's command-line-tools Clang may omit the libFuzzer runtime. On a Homebrew LLVM
installation, configure the fuzz preset once with
`-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++`.

## Local control and multi-server development

The IPC wire format is a four-byte network-order payload length followed by UTF-8 JSON.
Both directions are limited to 1 MiB. The public protocol layer performs strict schema
validation before a request reaches a method handler, and the Unix-domain-socket layer
handles concurrent single-request sessions without exposing SQLite to the CLI. Handler
execution is bounded to four worker threads and remains covered by each session's
absolute deadline. Stage 4 adds the daemon control service, separate credential store,
all resource commands, JSON output, stable exit codes, and Token input handling.

Exercise a real daemon-status round trip in a private directory:

```bash
runtime_root="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
runtime_dir="$(mktemp -d "$runtime_root/minitun.XXXXXX")"
build/dev/minitund \
  --socket "$runtime_dir/minitun.sock" \
  --database "$runtime_dir/state.db" \
  --credentials "$runtime_dir/credentials.db" \
  --tls-ca /path/to/server-ca.crt \
  --io-threads 4
# In another terminal:
build/dev/minitun --socket "$runtime_dir/minitun.sock" daemon status
build/dev/minitun --socket "$runtime_dir/minitun.sock" \
  server add example.com:2333 --name primary
build/dev/minitun --socket "$runtime_dir/minitun.sock" \
  tun add primary 22 6000 --name ssh
```

Run only IPC-focused tests with:

```bash
ctest --test-dir build/dev --output-on-failure \
  -R '(Ipc|Frame|Dispatcher|Credential|DaemonControl|cli-daemon)'
```

The IPC tests use isolated, physically resolved temporary socket paths. Decoder tests
cover partial and coalesced frames; transport tests cover malformed-client isolation,
deadlines, concurrent requests, single-request connections, pool shutdown, permissions,
trusted path ancestry, serialized startup, and cleanup. The CLI/daemon integration test
also exercises CRUD, JSON, restart recovery, PTY no-echo input, Token leak scanning,
tombstone filtering, and every documented exit-code class.

`integration.multi-server-sessions` generates a temporary CA/certificate, starts two
independent public servers, verifies simultaneous online state and failure isolation,
then restarts one server and the daemon to prove automatic recovery.

## Persistence development

The storage implementation uses the system SQLite3 library in both dependency modes.
Its default production path is `/var/lib/minitun/state.db`, but unit tests create
isolated temporary database files and do not touch that location.

Run only the storage and recovery tests after a persistence change with:

```bash
ctest --test-dir build/dev --output-on-failure -R '(Storage|Recovery|Credential)'
```

The tests cover fresh and repeated migration, refusal of future, drifted, or malformed
schemas, migration rollback, connection policy, transaction commit/rollback/isolation,
repository validation and constraints, monotonic timestamps, tombstone behavior,
restart-state recovery, credential permissions and CRUD, unsafe links and parent
directories, and concurrent daemon mutations.

`minitund` accepts `--database` and `--credentials`, opens both stores, runs state
recovery, checks credential references, and only then starts IPC. Tests must pass paths
inside private temporary directories; the program does not create production parent
directories.
