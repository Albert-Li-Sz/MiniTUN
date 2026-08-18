# P2P NAT Traversal Design and Implementation

> Status: v1.2.0 completes TCP simultaneous open (server-assisted, no STUN/TURN) and adds
> real-NAT validation: a new `worker_observed_endpoint` capability (`1ULL << 10`) makes the
> server report the Worker's own observed address back in START_RELAY, so the offer
> advertises a NAT-reachable candidate; an e2e test builds a dual-EIM topology with
> netns/iptables and verifies direct punch-through. This document describes the protocol
> design, the trigger conditions, and the remaining validation items.

## Current state

The P2P direct path only works when "both sides are routable": the daemon hands the peer
the address its Worker peer sees (the address from the server's perspective) as the direct
candidate. When either side sits behind NAT with no port forwarding, the candidate is
unreachable and the peer automatically falls back to the authenticated TLS relay. This is
the biggest functional gap for an "intranet penetration" product.

## Goal

Without changing the TCP wire image, without introducing STUN/TURN infrastructure, and
without significantly increasing resource usage, let the most common NAT combination (both
ends Endpoint-Independent Mapping) interconnect directly via TCP simultaneous open (TCP
SO); still fall back to relay on failure.

Explicitly out of scope:

- no full STUN/TURN/ICE stack — keep the minimal footprint and single-server architecture;
- no UDP hole punching — UDP keeps the authenticated Worker relay path; since
  v1.1.1 the P2P path itself may optionally carry UDP datagram records with a
  2-byte length prefix (direct and relay paths alike), still keeping the "one
  relay per one authenticated Worker" model and no new ports;
- no new external ports or listeners — reuse the existing public tunnel listener.

## Proposal: server-assisted TCP simultaneous open

### 1. Observed-address exchange (protocol extension)

The p2p branch of `START_RELAY` already carries the tunnel/connection IDs. Add two optional
fields:

- `peer_observed_endpoint`: the peer's public address as observed by the server (obtained
  from the peer's bootstrap TCP connection);
- `worker_observed_endpoint`: the daemon Worker's public address as observed by the server
  (obtained from the Worker's TLS connection, i.e. the existing direct candidate).

After a "symmetric-NAT precheck", both sides each initiate an outbound connect to the
other's observed address from the **same local port** (SO_REUSEADDR bound to the same local
port as the listening/worker socket), while keeping the existing inbound accept and relay
fallback. TCP SO needs no new wire frame: after success both ends see an ordinary TCP
connection, which then goes through the existing MTPD + token + TLS-PSK upgrade.

### 2. Trigger conditions and fallback

- only attempt SO when `mode == p2p` and the peer's ordinary connect fails within
  `--direct-timeout`;
- SO and relay fallback race in parallel; whichever establishes first wins; close the relay
  side immediately once direct establishes;
- when one end does not support the extension (no capability), behavior is exactly the same
  as today.

### 3. Capability negotiation

Add a `tcp_simultaneous_open` capability bit (`1ULL << 8`). The observed-address fields are
only sent after both sides advertise it; old implementations ignore the unknown capability
and stay wire-compatible.

### 4. Implemented and what remains to validate

v1.1 implements the SO data plane per this design: after an ordinary direct candidate
fails, the peer sends an `MTPS` request over the relay; the daemon initiates an outbound
connect to the peer's server-observed endpoint from the same local port as the direct
listener; once both connects cross, the existing `MTPD` + token + TLS 1.3 PSK upgrade
runs; failure falls back to the relay exactly as before. The peer retries in 200ms steps
within `--direct-timeout`, tolerating the window where its first SYN arrives before the
peer mapping forms. The `tcp_simultaneous_open` capability (`1ULL << 8`) is negotiated
between daemon and server; the connector enables `--simultaneous-open` by default and
needs `--no-simultaneous-open` against v1.0 daemons (old hosts treat `MTPS` as an invalid
control frame).

Unit coverage includes: loopback simultaneous open on both sides (no listener), SO socket
binding to the listener port with the ephemeral fallback, and the full peer protocol flow
(direct failure → `MTPS` → SO establishment → `MTPD` handshake → TLS failure → confirmed
relay fallback). v1.2.0 adds the `worker_observed_endpoint` capability (`1ULL << 10`): the
server reports the Worker's own observed address back in START_RELAY and the daemon
advertises that observed address (not its private local address) in the offer, so a NAT'd
Worker still offers a reachable candidate. The `integration.nat-traversal` e2e test builds
a "public network + two independent EIM NATs + two clients" topology with netns/iptables
and verifies that TCP SO selects the direct path through dual EIM NAT.

### 5. Acceptance criteria (remaining bar)

1. ✅ under dual EIM NAT, direct establishes and the data path uses TLS-PSK encryption (as
   today; validated by the `integration.nat-traversal` netns/iptables topology);
2. under symmetric NAT, no NAT and single-sided NAT combinations, the relay fallback time
   is no worse than today;
3. ports bound for SO reusing the same local port are correctly released after server
   restart and generation change;
4. ✅ new e2e tests pass against a controllable netns/iptables topology
   (`integration.nat-traversal`);
5. audit logs record the chosen path and failure reason, with no sensitive information
   other than addresses.

## Alternative routes (recorded, not adopted)

- **QUIC hole punching**: higher traversal rate, but introduces a full QUIC stack,
  conflicting with "minimal footprint".
- **Deploy a TURN relay**: server-side TURN relay covers symmetric NAT, but turns the
  server into a bandwidth aggregation point and changes the security and capacity model.
- **IPv6 first**: guiding users to IPv6 (no NAT) as a "zero-cost hole punch" can be
  recommended in the docs first; it does not preclude a future SO approach.
