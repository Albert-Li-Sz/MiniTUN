---
title: Changelog
---

# Changelog

The complete version history is governed by the
[CHANGELOG.md](https://github.com/Albert-Li-Sz/MiniTUN/blob/main/CHANGELOG.md) at the
repository root. This page keeps the recent-version summary used by the website, so you can
quickly learn the latest capabilities from the docs site.

## [1.1.1] - 2026-08-15

- P2P paths add UDP forwarding (`minitun-p2p --udp`) on both the direct and relay paths.

## [1.1.0] - 2026-08-15

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
The current P2P implementation does not do ICE/STUN/TURN/NAT hole punching; the direct
path is encrypted via TLS 1.3 PSK; when a candidate is unreachable it automatically falls
back to the authenticated TLS relay.
:::

::: tip Released
`v1.0.0` was released on 2026-08-13 and is the first formal release of this generation of
source code. All previous v0.x and old release records were deleted, and the public
history restarts from this version. See the [Installation Guide](/en/installation) for
installation.
:::
