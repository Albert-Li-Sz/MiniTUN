# MiniTun

MiniTun is an independently implemented TCP reverse-tunnelling system for Linux.
It is written in C++20 and is intentionally not compatible with the FRP protocol.

The repository is currently at **development stage 3**. In addition to the common and
SQLite layers, it contains a bounded local IPC protocol and Unix-domain-socket
transport. Requests use a four-byte network-order length prefix followed by strict
UTF-8 JSON. The daemon accepts concurrent local clients, dispatches registered methods,
contains malformed input and handler exceptions to the affected request or connection,
and creates its socket with mode `0660` in a daemon-owned runtime directory.

`minitun daemon status` now performs a real IPC round trip to `minitund`. This is still
not a deployable tunnel service: the full CLI, credential-material storage, remote
sessions, TLS, TCP relay, service installation, and packaging remain for later stages.
The daemon does not yet open or recover the database from its entry point. SQLite
stores only an opaque `credential_ref`; tokens, private keys, and other credential
material must never be placed in that field.

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
runtime_dir="$(mktemp -d)"
build/dev/minitund --socket "$runtime_dir/minitun.sock"
# In another terminal:
build/dev/minitun --socket "$runtime_dir/minitun.sock" daemon status
build/dev/minitund --version
build/dev/minitun-server --version
```

The production IPC path is `/run/minitun/minitun.sock`. The deployment account is
`minitun:minitun`; until the installation stage creates that account and runtime
directory, use a private temporary directory and override `--socket` as shown above.
The daemon must run as the intended socket owner.

See [development notes](docs/development.md) and the
[architecture overview](docs/architecture.md) for the current scope.

## License

MiniTun is available under the MIT License. See [LICENSE](LICENSE).
