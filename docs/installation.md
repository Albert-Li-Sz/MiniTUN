# Installation

Binary installation rules and Linux systemd integration are scheduled for stage 13.
Until then, use binaries directly from a verified build directory.

For a FetchContent developer build:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

OpenSSL 3 and SQLite3 must already be installed. The release preset additionally expects
system packages for CLI11, standalone Asio, nlohmann/json, spdlog, and GoogleTest.
