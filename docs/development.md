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
