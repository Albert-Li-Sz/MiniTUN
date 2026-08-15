---
layout: home

hero:
  name: MiniTun
  text: The minimal-footprint intranet penetration tool
  tagline: TCP, UDP, SOCKS5, P2P and two stable SDKs share a secure Remote Protocol v2 control plane; no web GUI and no extra runtime dependencies.
  image:
    src: /logo.svg
    alt: MiniTun
  actions:
    - theme: brand
      text: Installation Guide
      link: /en/installation
    - theme: alt
      text: Browse the CLI
      link: /en/cli

features:
  - title: Secure transport
    details: TLS 1.2+ and Protocol v2; per-client PSK, optional certificate binding, port ACL, quotas, replay protection and authentication rate limiting.
  - title: Durable control plane
    details: schema v5 stores stable identity, transport mode, configuration revision and ownership; a generation-scoped reconciler converges deterministically after disconnects and out-of-order responses.
  - title: Multiple server sessions
    details: A single client daemon can connect to several public servers at once, with sessions, tunnels and worker pools isolated from one another.
  - title: Four data planes
    details: TCP, UDP datagram, SOCKS5 CONNECT, and a P2P direct path with automatic TLS relay fallback.
  - title: Minimal footprint
    details: No web GUI and no scripting runtime; a single C++20 daemon process plus embedded SQLite, with sandbox-hardened systemd units, suitable for routers, NAS devices and edge hardware.
  - title: SDK
    details: A local-control C11 ABI / C++20 RAII SDK, plus a standalone Remote Protocol v2 C++ codec/decoder SDK.
  - title: Production operations
    details: Both client and server can expose health, readiness and Prometheus metrics, with auditing, hot reload, diagnostics and paired backups.
  - title: Multi-architecture delivery
    details: Client/Server/SDK DEB, RPM and multi-arch OCI images, plus SBOM, checksums, keyless signing and provenance.
  - title: Continuous verification
    details: CI covers GCC, Clang, Sanitizers, coverage, clang-tidy, CodeQL, fuzzing, ABI, fault injection, packaging and standalone performance validation.
---

## How it works

MiniTun consists of the public server <code>minitun-server</code>, the client daemon
<code>minitund</code>, the command-line tool <code>minitun</code>, the P2P connector
<code>minitun-p2p</code>, and two SDKs. The CLI and the local SDK manage the daemon only
through a permission-protected Unix IPC; remote control and Workers use Protocol v2 over
TLS.

<div class="minitun-flow">
  <div class="flow-node">Public TCP / UDP / SOCKS5 / P2P client</div>
  <div class="flow-link">→</div>
  <div class="flow-node">minitun-server<br>public port</div>
  <div class="flow-link">⇄ TLS Worker</div>
  <div class="flow-node">minitund<br>internal service</div>
</div>

## Production deployment

Before deploying, prepare a public server, an internal host that can reach the target
service, a server domain name with a valid TLS certificate, and the TCP/UDP ports you
actually need to expose.

| Package | Runtime | Release architectures |
| --- | --- | --- |
| DEB | Debian/Ubuntu | <code>amd64</code>, <code>arm64</code>, <code>armhf</code>, <code>riscv64</code> |
| RPM | Fedora/RHEL family | <code>x86_64</code>, <code>aarch64</code>, <code>armv7hl</code>, <code>riscv64</code> |
| OCI | Docker / containerd | <code>linux/amd64</code>, <code>linux/arm64</code>, <code>linux/arm/v7</code>, <code>linux/riscv64</code> |

::: tip Recommended path
In production, prefer the DEB/RPM packages or OCI images from Releases. If the target
version has not been released yet, build and verify the packages in a trusted build
environment as described in the [Development Guide](/en/development).
:::

## Common commands

<div class="mini-command">

~~~bash
minitun daemon status
minitun daemon identity --json
minitun health
minitun readiness
minitun metrics
minitun doctor --json --checkpoint
minitun server list
minitun server inspect primary --json
minitun tun list primary
minitun tun inspect web --json
minitun-p2p tunnel.example.com:6003 --listen 127.0.0.1:6501
~~~

</div>

For the complete command reference, JSON output and exit codes, see the [CLI documentation](/en/cli).

## Documentation index

- [Installation Guide](/en/installation): signature verification, DEB/RPM/OCI installation, source builds, first deployment and uninstall.
- [Command Line Interface](/en/cli): lifecycle commands, PSK input, JSON output and exit codes.
- [Configuration & Policies](/en/configuration): per-client policies and declarative resources.
- [System Architecture](/en/architecture): schema v5, the reconciler, sessions, Workers and the multiple data planes.
- [Remote Protocol v2](/en/protocol): capability negotiation, authentication, registration and data relay.
- [SDK](/en/sdk): local-control C11/C++20 APIs and the Remote Protocol C++20 codec/decoder.
- [Operations & Observability](/en/operations): admin endpoints, metrics, auditing and backups.
- [Let's Encrypt Certificates](/en/letsencrypt): certbot recipes for automatic issuance and renewal.
- [Performance & Soak Validation](/en/performance): optional three-run benchmarks, 24-hour stress and 7-day soak.
- [Development Guide](/en/development): source builds, local demos, testing, packaging, releases and troubleshooting.
- [Changelog](/en/changelog): recent version change records.
