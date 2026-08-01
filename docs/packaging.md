# Packaging

MiniTun uses CPack to generate two component packages:

| Package | CMake component | Contents |
| --- | --- | --- |
| `minitun-client` | `Client` | `minitun`, `minitund`, client unit, sysusers definition, and man pages |
| `minitun-server` | `Server` | `minitun-server`, server unit, sysusers definition, man page, and configuration README |

The production package layout uses `/usr` for installed programs and shared data,
`/etc` for administrator configuration, `/usr/lib/systemd/system` for units, and
`/usr/lib/sysusers.d` for account definitions. Public development headers are not part
of either runtime package.

## Build DEB packages

On Ubuntu 22.04 or newer, install the build dependencies listed in
[installation.md](installation.md), plus `dpkg-dev`, `fakeroot`, and `file`, then run:

```bash
cmake --preset package-deb
cmake --build --preset package-deb --parallel
ctest --test-dir build/package-deb --output-on-failure
cpack --config build/package-deb/CPackConfig.cmake -G DEB
packaging/tests/verify-deb.sh build/package-deb
```

The generated files use native Debian names:

```text
minitun-client_0.1.0_amd64.deb
minitun-server_0.1.0_amd64.deb
```

Inspect a package directly with:

```bash
dpkg-deb -I build/package-deb/minitun-client_0.1.0_amd64.deb
dpkg-deb -c build/package-deb/minitun-client_0.1.0_amd64.deb
```

## Build RPM packages

On Fedora, install the dependencies listed in [installation.md](installation.md), plus
`rpm-build`, then run:

```bash
cmake --preset package-rpm
cmake --build --preset package-rpm --parallel
ctest --test-dir build/package-rpm --output-on-failure
cpack --config build/package-rpm/CPackConfig.cmake -G RPM
packaging/tests/verify-rpm.sh build/package-rpm
```

The generated files use native RPM names:

```text
minitun-client-0.1.0-1.x86_64.rpm
minitun-server-0.1.0-1.x86_64.rpm
```

Inspect a package directly with:

```bash
rpm -qip build/package-rpm/minitun-client-0.1.0-1.x86_64.rpm
rpm -qlp build/package-rpm/minitun-client-0.1.0-1.x86_64.rpm
```

## Lifecycle behavior

Both package formats run `systemd-sysusers` during installation and request a
`systemctl daemon-reload` when systemd is available. They do not enable or start a
service automatically, so installation is safe in containers and administrators can
provision credentials before first start.

MiniTun packages never contain `server.crt`, `server.key`, or `token`, and do not
overwrite administrator-provided TLS or Token files. Reinstallation, upgrade, and
ordinary removal preserve `/var/lib/minitun` and `/var/lib/minitun-server`. Debian
purge removes those state directories; RPM removal preserves them because RPM has no
separate purge operation.

Run the reusable smoke tests inside clean amd64/x86_64 containers after copying or
mounting the generated packages at `/packages`:

```bash
packaging/tests/smoke-deb.sh /packages
packaging/tests/smoke-rpm.sh /packages
```

The tests install both packages, verify all three version commands, confirm the unit
and sysusers files, simulate an upgrade, remove the packages, and verify the documented
state-retention policy without starting systemd.
