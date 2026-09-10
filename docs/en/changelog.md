---
title: Changelog
---

# Changelog

The complete version history is governed by the
[CHANGELOG.md](https://github.com/Albert-Li-Sz/MiniTUN/blob/main/CHANGELOG.md) at the
repository root. This page keeps the recent-version summary used by the website, so you can
quickly learn the latest capabilities from the docs site.

## [1.2.3] - 2026-09-11

- The server enforces numeric loopback SOCKS5 registration, including clients bypassing the daemon.
- P2P direct contexts reuse the shared explicit TLS policy, retaining TLS 1.3 and one-time token external PSK.
- UDP records reject declared payloads above 65,507 bytes before reading, with oversized wire-record regressions.
- Current docs now reflect schema v6, direct TLS encryption, TCP simultaneous open and the added capabilities.

## [1.2.2] - 2026-09-09

- Server startup compares `--max-total-connections` with the visible memory ceiling and warns
  when the default cannot fit it, instead of letting an OOM kill report the mismatch.
- TLS cipher policy is pinned explicitly: TLS 1.2 ECDHE+AEAD only, TLS 1.3 AEAD suites only.
- Authenticated admin endpoints reject a `Host` that does not name the listener
  (`421 Misdirected Request`), closing the DNS-rebinding path.
- The authentication replay cache expires entries in O(1) instead of scanning on every
  authentication.
- The Remote Protocol SDK ABI gate now compares a symbol baseline instead of a symbol count.
- Static musl builds move to OpenSSL 3.5 LTS; the repository drops a truncated `sqlite.zip`.

## [1.2.1] - 2026-09-05

- The TLS listener limits accept rate and global/per-IP unauthenticated connections before
  allocating TLS objects, with timer backoff and temporary blocks for repeated source failures.
- TLS and application authentication share an absolute deadline; authentication and cleanup
  release the pending quota.
- Repeated TLS warnings are limited to one every five seconds, with pending-handshake,
  TLS-failure, and admission-rejection metrics.
- Regression tests cover real TLS/plaintext floods, slow connections, quota release,
  heartbeats, and shutdown.
- Fixes the public bridge topology in the Linux NAT integration test and prints network
  and process diagnostics on failure.
- Fixes P2P punch port reuse and paces immediate failures to prevent CPU/SYN floods.

## [1.2.0] - 2026-08-18

- P2P NAT hole punching gains the `worker_observed_endpoint` capability, plus a
  netns/iptables dual-EIM-NAT e2e test verifying TCP simultaneous open direct
  punch-through.
- Daemon metrics add UDP-over-P2P datagram/byte counters (`minitun_p2p_udp_*_total`).
- Adds an admin HTTP parsing fuzz target with a persistent corpus.
- The quality gate adds Chinese/English documentation parity enforcement.

## [1.1.1] - 2026-08-15

- P2P paths add UDP forwarding (`minitun-p2p --udp`) on both the direct and relay paths.

## [1.1.0] - 2026-08-15

- The state database upgrades to schema v6 with `tunnels.proxy_protocol` disabled by default.
  v4/v5 data migrates transactionally to v6, preserving resources, configuration and credential
  references; rollback requires restoring paired backups from before the upgrade.

- tcp tunnels support PROXY protocol v1 headers (`--proxy-protocol`), staying
  byte-compatible with older peers.
- `minitun-server` gains a `/v1/*` client policy management API (list/create/update/
  delete/PSK rotation) with a rotation grace window that keeps sessions alive.
- Client policies add source CIDR whitelisting and per-source connection rates; systemd
  units gain memory/task limits.
- Releases add musl fully static binary archives (`static.yml`).
- P2P gains server-assisted TCP simultaneous open (NAT hole punching); the
  `--simultaneous-open` connector flag defaults on and falls back to relay.
- The documentation site adds an English language (Chinese remains the default).

## [1.0.0] - 2026-08-13

### Major changes

- Four tunnel modes: TCP, UDP datagram, SOCKS5 no-auth CONNECT and P2P direct/relay
  fallback; Remote Protocol v2 keeps the old TCP wire image via capability negotiation and
  an appended mode byte.
- A standalone `minitun-p2p` connector; per-client PSK, port ACL, quotas and auditing.
- `libminitun-remote-protocol.so.1` C++20 codec/decoder/helper SDK; the local-control
  C11/C++20 SDK supports creating/updating all four modes in a `struct_size`-compatible
  way.
- State database schema v5, with automatic migration of historical v3/v4 data; no web GUI
  and no scripting runtime, focused on a minimal footprint, suitable for routers, NAS
  devices and edge hardware.

::: warning P2P boundary
Current P2P supports server-assisted TCP simultaneous open; ICE/STUN/TURN and UDP hole
punching are not implemented. The direct path encrypts application data using TLS 1.3 with
a one-time token as external PSK; failed direct connections fall back to the authenticated
TLS relay. TLS upgrades and TCP hole punching were introduced in v1.1.0, with NAT candidate
observed addresses added in v1.2.0.
:::

::: tip Released
`v1.0.0` was released on 2026-08-13 and is the first formal release of this generation of
source code. All previous v0.x and old release records were deleted, and the public
history restarts from this version. See the [Installation Guide](/en/installation) for
installation.
:::
