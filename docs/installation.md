# Installation

MiniTun installs three CMake components:

| Component | Contents |
| --- | --- |
| `Client` | `minitun`, `minitund`, client systemd/sysusers files, and client man pages |
| `Server` | `minitun-server`, server systemd/sysusers files, and the server man page |
| `Development` | Public C++ headers for source-level integration and inspection |

DEB and RPM packages are described in [packaging.md](packaging.md). This page covers a
direct CMake installation on a Linux host.

## Build prerequisites

Install CMake 3.22 or newer, Ninja, a C++20 compiler, and development packages for
OpenSSL 3, SQLite3, CLI11, standalone Asio, nlohmann/json, spdlog, and GoogleTest.

Debian or Ubuntu package names are typically:

```bash
sudo apt-get install \
  build-essential cmake ninja-build \
  libssl-dev libsqlite3-dev libcli11-dev libasio-dev \
  nlohmann-json3-dev libspdlog-dev libgtest-dev
```

Fedora package names are typically:

```bash
sudo dnf install \
  gcc-c++ cmake ninja-build openssl-devel sqlite-devel \
  CLI11-devel asio-devel json-devel spdlog-devel gtest-devel
```

## Build and verify

Configure the install prefix as `/usr` so the generated paths match the packaged Linux
layout:

```bash
cmake -S . -B build/install -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_SYSCONFDIR=/etc \
  -DMINITUN_USE_SYSTEM_DEPS=ON \
  -DMINITUN_BUILD_TESTS=ON
cmake --build build/install --parallel
ctest --test-dir build/install --output-on-failure
```

Inspect an unprivileged staged installation before touching the host:

```bash
DESTDIR="$PWD/build/stage" cmake --install build/install --component Client
DESTDIR="$PWD/build/stage" cmake --install build/install --component Server
find build/stage/usr -type f -o -type l
```

Install all runtime components and create the service accounts:

```bash
sudo cmake --install build/install --component Client
sudo cmake --install build/install --component Server
sudo systemd-sysusers /usr/lib/sysusers.d/minitun.conf
sudo systemd-sysusers /usr/lib/sysusers.d/minitun-server.conf
sudo systemctl daemon-reload
```

The programs do not create `/run/minitun`, `/var/lib/minitun`,
`/run/minitun-server`, or `/var/lib/minitun-server`. systemd creates them from the
`RuntimeDirectory` and `StateDirectory` directives with mode `0750`.

## Configure the public server

MiniTun never ships or overwrites TLS material or Tokens. Install administrator-owned
files before enabling the server:

```bash
sudo install -d -m 0750 -o root -g minitun-server /etc/minitun-server
sudo install -m 0644 server.crt /etc/minitun-server/server.crt
sudo install -m 0600 -o minitun-server -g minitun-server \
  server.key /etc/minitun-server/server.key
sudo install -m 0600 -o minitun-server -g minitun-server \
  token /etc/minitun-server/token
```

The installed service listens on `0.0.0.0:2333` and permits public tunnel ports
`6000-6999`. Override it without editing the vendor unit:

```bash
sudo systemctl edit minitun-server
```

An override can replace `ExecStart` after first clearing it:

```ini
[Service]
ExecStart=
ExecStart=/usr/bin/minitun-server --foreground \
  --listen 0.0.0.0:4433 \
  --allow-ports 10000-10999 \
  --tls-cert /etc/minitun-server/server.crt \
  --tls-key /etc/minitun-server/server.key \
  --token-file /etc/minitun-server/token
```

## Start services

```bash
sudo systemctl enable --now minitun-server.service
sudo systemctl enable --now minitund.service
```

The client socket is mode `0660` and belongs to `minitun:minitun`. Add an authorized
operator to the group, then start a new login session:

```bash
sudo usermod -aG minitun "$USER"
```

Verify the installation:

```bash
minitun version
/usr/libexec/minitun/minitund --version
minitun-server --version
systemctl status minitund.service minitun-server.service
journalctl -u minitund.service -u minitun-server.service
```

## Installed layout

```text
/usr/bin/minitun
/usr/libexec/minitun/minitund
/usr/lib/systemd/system/minitund.service
/usr/lib/sysusers.d/minitun.conf
/usr/share/man/man1/minitun.1
/usr/share/man/man8/minitund.8

/usr/bin/minitun-server
/usr/lib/systemd/system/minitun-server.service
/usr/lib/sysusers.d/minitun-server.conf
/usr/share/man/man8/minitun-server.8
/etc/minitun-server/README
```

Normal package removal and upgrades preserve state and credentials. The direct CMake
installer records installed files in `build/install/install_manifest.txt`; state under
`/var/lib` is never part of that manifest.
