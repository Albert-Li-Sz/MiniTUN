# Development Guide

This document covers MiniTun source builds, local runs, non-package installation, testing,
packaging, releases and development troubleshooting. For production, use the package and
systemd deployment flow described in the [project home](/en/).

## Development environment

Base requirements:

- Linux or macOS; full systemd and package testing requires Linux;
- CMake 3.22 or higher;
- Ninja;
- a C++20-capable GCC or Clang;
- OpenSSL 3, SQLite3 and Python 3;
- Node.js 22.12+ and npm (only when modifying or verifying the docs site);
- network access to FetchContent upstream dependencies.

Minimal development dependencies on Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends --yes \
  build-essential cmake curl git libsqlite3-dev libssl-dev ninja-build \
  openssl pkg-config python3
```

Minimal development dependencies on Fedora:

```bash
sudo dnf install \
  cmake curl gcc-c++ git ninja-build openssl openssl-devel \
  pkgconf-pkg-config python3 sqlite-devel
```

The `dev` preset fetches CLI11, standalone Asio, nlohmann/json, spdlog and GoogleTest at
verified pinned versions. The `release` preset uses distro-provided system dependencies;
when using it, also install the matching CLI11, Asio, nlohmann/json, spdlog and GoogleTest
development packages.

## Build and test

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Generated programs are under `build/dev/`:

```text
build/dev/minitun
build/dev/minitund
build/dev/minitun-server
build/dev/minitun-p2p
build/dev/libminitun-client.so.1  # Linux; the corresponding dylib on macOS
build/dev/libminitun-remote-protocol.so.1
```

Common CMake options:

| Option | Effect |
| --- | --- |
| `MINITUN_USE_SYSTEM_DEPS` | use system dependencies instead of pinned FetchContent dependencies |
| `MINITUN_BUILD_TESTS` | build CTest tests |
| `MINITUN_BUILD_FUZZERS` | build libFuzzer targets |
| `MINITUN_ENABLE_ASAN` | enable AddressSanitizer |
| `MINITUN_ENABLE_UBSAN` | enable UndefinedBehaviorSanitizer |
| `MINITUN_ENABLE_TSAN` | enable ThreadSanitizer |
| `MINITUN_ENABLE_LTO` | enable link-time optimization |
| `MINITUN_ENABLE_COVERAGE` | generate line/branch coverage data for core code |
| `MINITUN_ENABLE_FAULT_INJECTION` | enable test-only crash failpoints |
| `MINITUN_WARNINGS_AS_ERRORS` | treat project-code warnings as errors |
| `MINITUN_BUILD_PACKAGES` | enable CPack package generation |
| `MINITUN_PACKAGE_VERSION` | set the stable or candidate package version |

To run only one group of tests, use a CTest regular expression:

```bash
ctest --test-dir build/dev --output-on-failure -R '(Storage|Recovery|Credential)'
ctest --test-dir build/dev --output-on-failure -R '(Ipc|Dispatcher|DaemonControl)'
ctest --test-dir build/dev --output-on-failure -R '(MultiServer|Tunnel|Worker|Relay)'
```

## Full local demo

The following flow runs entirely on this machine and is only for development and functional
verification.

### 1. Create temporary credentials

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
install -d -m 0700 "$MINITUN_DEMO_DIR"

openssl req -x509 -newkey rsa:3072 -nodes \
  -keyout "$MINITUN_DEMO_DIR/server.key" \
  -out "$MINITUN_DEMO_DIR/server.crt" \
  -days 1 \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'
openssl rand -hex 32 >"$MINITUN_DEMO_DIR/client.psk"
chmod 0600 "$MINITUN_DEMO_DIR/server.key" "$MINITUN_DEMO_DIR/client.psk"
```

### 2. Start the client daemon

Run in the first terminal:

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
build/dev/minitund \
  --foreground \
  --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  --database "$MINITUN_DEMO_DIR/state.db" \
  --credentials "$MINITUN_DEMO_DIR/credentials.db" \
  --tls-ca "$MINITUN_DEMO_DIR/server.crt"
```

### 3. Create the client policy and start the server

Run in the second terminal:

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"
bash tests/integration/write_client_policy.sh \
  build/dev/minitun "$MINITUN_DEMO_DIR/minitun.sock" \
  "$MINITUN_DEMO_DIR/clients.json" "$MINITUN_DEMO_DIR/client.psk"

build/dev/minitun-server \
  --foreground \
  --listen 127.0.0.1:2333 \
  --tls-cert "$MINITUN_DEMO_DIR/server.crt" \
  --tls-key "$MINITUN_DEMO_DIR/server.key" \
  --clients-config "$MINITUN_DEMO_DIR/clients.json"
```

### 4. Start the target service

Run in the third terminal:

```bash
python3 -m http.server 8080 --bind 127.0.0.1
```

### 5. Register and verify the tunnel

Run in the fourth terminal:

```bash
export MINITUN_DEMO_DIR="$PWD/build/demo-runtime"

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  server add localhost:2333 --name demo

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  server login demo --psk-stdin <"$MINITUN_DEMO_DIR/client.psk"

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  tun add demo 8080 6000 --name demo-http

build/dev/minitun --socket "$MINITUN_DEMO_DIR/minitun.sock" \
  tun inspect demo-http --json

curl http://127.0.0.1:6000/
```

Tunnel synchronization is asynchronous; if the state is still `pending`, re-run
`tun inspect` later. When done, stop the three foreground processes and delete the one-time
credentials and state in `build/demo-runtime`.

## CMake staging and direct install

Build a Release configuration with system dependencies:

```bash
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
```

Before touching system directories, do an unprivileged staged install and inspect the file
layout:

```bash
DESTDIR="$PWD/build/stage" \
  cmake --install build/release --prefix /usr --component Client
DESTDIR="$PWD/build/stage" \
  cmake --install build/release --prefix /usr --component Server
DESTDIR="$PWD/build/stage" \
  cmake --install build/release --prefix /usr --component ClientLibrary
DESTDIR="$PWD/build/stage" \
  cmake --install build/release --prefix /usr --component ClientDevelopment
find build/stage/usr -type f -o -type l
```

When a development host needs a direct install:

```bash
sudo cmake --install build/release --prefix /usr --component Client
sudo cmake --install build/release --prefix /usr --component Server
sudo cmake --install build/release --prefix /usr --component ClientLibrary
sudo cmake --install build/release --prefix /usr --component ClientDevelopment
sudo systemd-sysusers /usr/lib/sysusers.d/minitun.conf
sudo systemd-sysusers /usr/lib/sysusers.d/minitun-server.conf
sudo systemctl daemon-reload
```

systemd creates the runtime and state directories from the unit's `RuntimeDirectory` and
`StateDirectory`. A direct install does not auto-generate TLS material and does not
auto-enable services. Production hosts should prefer the DEB/RPM flow in the README so the
package manager tracks files and lifecycle.

## Sanitizers and fuzzing

Combined ASan and UBSan build:

```bash
cmake --preset asan
cmake --build --preset asan --parallel 2
ctest --preset asan
```

Standalone UBSan and TSan builds:

```bash
cmake --preset ubsan
cmake --build --preset ubsan --parallel 2
ctest --preset ubsan

cmake --preset tsan
cmake --build --preset tsan --parallel 2
ctest --preset tsan
```

Build all fuzz targets with Clang and libFuzzer:

```bash
cmake --preset fuzz
cmake --build --preset fuzz --parallel 2
for target in remote_frame ipc_frame ipc_json endpoint port_range admin_http; do
  "build/fuzz/minitun_${target}_fuzz" -runs=2000 -max_total_time=10
done
```

Apple Command Line Tools' Clang may not include the libFuzzer runtime. When using Homebrew
LLVM, append this on first configure:

```text
-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
```

## Building DEB and RPM

DEB builds require `dpkg-dev`, `fakeroot` and `file`:

```bash
cmake --preset package-deb
cmake --build --preset package-deb --parallel
ctest --test-dir build/package-deb --output-on-failure
cpack --config build/package-deb/CPackConfig.cmake -G DEB
packaging/tests/verify-deb.sh build/package-deb
```

RPM builds require Fedora system dependencies and `rpm-build`:

```bash
cmake --preset package-rpm
cmake --build --preset package-rpm --parallel
ctest --test-dir build/package-rpm --output-on-failure
cpack --config build/package-rpm/CPackConfig.cmake -G RPM
packaging/tests/verify-rpm.sh build/package-rpm
```

This produces `minitun-client`, `minitun-server`, `libminitun-client1` and
`libminitun-client-dev`/`libminitun-client-devel`. Client includes the CLI, daemon and P2P
connector; the runtime and development files of the two SDKs share the corresponding
library/devel packages. On a Linux development host with Docker available, you can continue
with clean-container smoke tests:

```bash
packaging/tests/smoke-deb.sh "$PWD/build/package-deb"
packaging/tests/smoke-rpm.sh "$PWD/build/package-rpm"
```

Ordinary upgrades and uninstalls keep the state directories; Debian purge removes them,
while RPM uninstall always keeps the state. Packages never carry or overwrite
administrator-provided certificates, private keys, PSKs or client policies.

## Cross-compiling DEB/RPM and OCI images

### Cross-compiling DEB/RPM

`package.yml` builds `arm64`/`armhf`/`riscv64` DEBs and `aarch64`/`armv7hl`/`riscv64` RPMs
on ubuntu-24.04 using the distro cross toolchains: `dpkg --add-architecture` (riscv64 also
adds the ports repository) installs `libssl-dev:<arch>` and `libsqlite3-dev:<arch>`, and
CMake generates target-architecture packages via `CMAKE_SYSTEM_PROCESSOR`,
`<triplet>-g++` and `CPACK_*_PACKAGE_ARCHITECTURE`. Cross DEBs use explicit `Depends`
(shlibdeps disabled), and cross RPMs rely on rpmbuild's ELF dependency scan to generate
soname-level `Requires`. Each new architecture installs and runs `minitun version` /
`minitund --version` / `minitun-server --version` in a QEMU container as a smoke test.

### OCI images

`packaging/oci/Dockerfile.server` and `Dockerfile.client` are based on
`debian:stable-slim`, directly copy the cross-built binaries (no compilation inside the
image) and run as non-root (UID 65532); the client image embeds system CAs via
`ca-certificates`. The OCI job in `package.yml` extracts binaries from the DEB artifacts,
builds images per architecture and pushes them to `ghcr.io/albert-li-sz/minitun-server` and
`ghcr.io/albert-li-sz/minitun-client`, then aggregates them with `docker manifest` into a
multi-arch manifest covering amd64/arm64/arm/v7/riscv64.

## CI and releases

| Workflow | Contents |
| --- | --- |
| `ci.yml` | Linux GCC/Clang, macOS compile, full CTest, SDK examples |
| `sanitizers.yml` | ASan, UBSan, TSan, PR fuzz smoke and nightly corpus fuzzing |
| `quality.yml` | reproducible docs build, clang-tidy, line ≥85% / branch ≥75% coverage, ABI/downstream checks |
| `codeql.yml` | GitHub CodeQL C/C++ scanning |
| `reliability.yml` | tunnel registration and high-latency reconciliation repeated 100 times |
| `performance.yml` | optional standalone 4 vCPU/8 GiB three-run benchmark, persistent systemd soak and OIDC evidence |
| `package.yml` | four-architecture DEB/RPM, QEMU install tests, multi-arch OCI and non-blocking vulnerability report |
| `release.yml` | RC continuity/frozen commit/P0-P1 gates, SBOM, signing, attestation and GitHub Release |
| `pages.yml` | VitePress docs build and publish |
| `static.yml` | musl fully static binaries (x86_64/aarch64) for release tags, attached to the release |

`main` branch packages use `MAJOR.MINOR.PATCH_pre<run-number>~<commit-number>`; they are
only for continuous acceptance. Release tags must be `vMAJOR.MINOR.PATCH` or
`vMAJOR.MINOR.PATCH-rc.NUMBER`, and the base version must match `CMakeLists.txt`.

### Normal release sequence

1. Release `v<version>-rc.1` and freeze protocol, schema, SDK ABI and features;
2. Fix blockers only and publish subsequent `rc.N` in order;
3. Confirm the final RC's required build, test, packaging and blocking security checks
   satisfy GA;
4. Confirm there are no open P0/P1 issues;
5. Create the annotated GA tag on the same commit as the final RC (e.g. `v1.0.1`).

`release.yml`'s GA gates require rc.1 and rc.2 to exist, all RCs to be ancestors of the GA
commit, and the final RC and GA to point to the same commit. RC version numbers are counted
independently per release line (e.g. the 1.0.1 line starts at `v1.0.1-rc.1` and does not
inherit the previous line's rc.N). Candidate example:

```bash
git tag -s v1.0.1-rc.1 -m "MiniTun v1.0.1-rc.1"
git push origin v1.0.1-rc.1
```

Any source change after the final RC still requires publishing a subsequent `rc.N`, because
GA must point to exactly the same commit as the final RC; this freeze rule is independent of
the optional performance/soak validation.

### First GA on a release line (history reset)

The project performed a public history reset on 2026-08-13: all old release records were
deleted and the current source was re-released as `v1.0.0`. The `v1.0.0` tag did not exist
yet, but the GA gate still required rc.1/rc.2. The approach was to create transient
candidates on the same commit and let GA build in parallel:

```bash
git tag -a v1.0.0-rc.1 -m "MiniTun v1.0.0-rc.1"
git tag -a v1.0.0-rc.2 -m "MiniTun v1.0.0-rc.2"
git tag -a v1.0.0     -m "MiniTun v1.0.0"
git push origin v1.0.0-rc.1 v1.0.0-rc.2 v1.0.0
```

GA's `release-gates` only requires those two tags to exist and point to the same commit, so
the three pipelines can be triggered at once. After GA succeeds, delete the transient RC
releases and tags, leaving only the final GA in the public history.

The three-run performance, 24-hour stress and 7-day soak can be run manually via
`performance.yml` to produce engineering validation records with OIDC attestation;
`release.yml` does not download or require these records, and their absence or failure never
blocks RC or GA. See the [Performance documentation](/en/performance) for the specific
start/collect commands.

OCI vulnerability scanning fully reports High/Critical findings in both RC and GA but does
not block release. The report stays in the Actions logs for release decisions and later base
image fixes; CodeQL, dependency audit and other security gates remain blocking.

Each architecture produces four packages — client, server, SDK runtime and SDK development
— so the full matrix is 16 DEBs and 16 RPMs, plus multi-arch OCI. Releases also include
SPDX/CycloneDX SBOMs, `SHA256SUMS`, a Sigstore bundle per blob and GitHub OIDC
provenance/attestation.

## Development troubleshooting

First check the version, process status and recent logs:

```bash
minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
systemctl status minitund.service minitun-server.service
journalctl -u minitund.service -u minitun-server.service --since '-10 min'
```

Common issues:

| Symptom | What to check |
| --- | --- |
| CLI exit code `3` | whether `minitund` is running, whether the socket is `0660`, whether the current user is in the `minitun` group |
| server cannot start | TLS certificate/key, `clients.json` owner/mode, whether each PSK is owned by the service account with mode `0600` |
| authentication failure | server SAN/CA, client policy ID, PSKs on both ends, optional certificate binding, system time and control-port firewall |
| tunnel stays `pending` | `server_actual_state`, `pending_reason`, `config_revision`, `last_synced_at`, control port, TLS and PSK |
| tunnel state `failed` | `permission_denied`, `resource_exhausted` or `remote_port_in_use` error codes |
| public port unreachable | local target, cloud security group and host firewall for the mapped port, tunnel state and connection quota |

During local development, the socket and databases must be in real directories owned by the
current user with mode `0700`. MiniTun rejects symlinks, unsafe parent directories, wrong
permissions, mode drift or future-version databases. Do not bypass checks by relaxing file
permissions or deleting the database; back up the files first, then locate the cause from
the logs.

`state.db` uses WAL mode, so the latest transactions during a run may live in
`state.db-wal`; `credentials.db` uses a DELETE journal with secure deletion. When checking
or backing up an active database, use the SQLite online backup interface or keep the main
file plus `-wal` and `-shm` together; never delete any sidecar file while `minitund` is
running. The logical result after a successful `remove` should be queried via
`minitun list/inspect` or a SQLite connection, not by comparing main-file timestamps alone.

Never submit PSKs, private keys, `credentials.db` or non-sanitized production data in
issues, logs or test data.
