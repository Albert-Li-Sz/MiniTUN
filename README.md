# MiniTun

MiniTun is an independently implemented TCP reverse-tunnelling system for Linux.
It is written in C++20 and is intentionally not compatible with the FRP protocol.

The repository is currently at **development stage 2**. In addition to the common
types and build baseline, it contains a versioned SQLite storage layer, transactional
server and tunnel repositories, and deterministic restart-state recovery. The default
state database path is `/var/lib/minitun/state.db`; opening a database migrates it to
schema version 1 and requires WAL mode, foreign keys, `synchronous=NORMAL`, and a
5-second busy timeout.

This is still not a deployable tunnel service. `minitund` links the storage layer, but
its startup path does not open or recover the database yet. IPC, user-facing server and
tunnel commands, credential-material storage, remote sessions, TLS, TCP relay, and
packaging remain for later stages. SQLite stores only an opaque `credential_ref`;
tokens, private keys, and other credential material must never be placed in that field.

## Build the current baseline

Ninja, CMake 3.22 or newer, a C++20 compiler, OpenSSL 3, and SQLite3 are required.
The developer preset downloads pinned releases of the remaining build dependencies.

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

## Available commands

```bash
build/dev/minitun --help
build/dev/minitun version
build/dev/minitund --version
build/dev/minitun-server --version
```

See [development notes](docs/development.md) and the
[architecture overview](docs/architecture.md) for the current scope.

## License

MiniTun is available under the MIT License. See [LICENSE](LICENSE).
