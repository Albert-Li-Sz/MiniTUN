# MiniTun

MiniTun is an independently implemented TCP reverse-tunnelling system for Linux.
It is written in C++20 and is intentionally not compatible with the FRP protocol.

The repository is currently at **development stage 1**. In addition to the build and CI
baseline, it contains validated endpoint and port-range types, cryptographically random
typed IDs, overflow-safe time helpers, move-only secure-string storage, structured
logging, and the common error/result model. TCP tunnelling, persistence, IPC, TLS, and
packaging are deliberately not implemented yet.

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
