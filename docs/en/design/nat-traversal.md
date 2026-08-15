# P2P NAT Traversal Design Proposal

> Status: design proposal (not implemented). This document describes how to add NAT
> traversal to P2P tunnels without breaking Remote Protocol v2, why it is not implemented
> now, and the acceptance criteria for when it is.

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
- no UDP hole punching or UDP-over-TCP encapsulation — keep the "one relay per one
  authenticated Worker" model;
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

### 4. Why not implement now

- **Cannot validate locally**: a macOS development machine cannot construct a real NAT
  environment and the CI runner has only one egress; blindly implementing would only
  deliver "compiles but with unknown traversal rate" code, violating this project's
  standard of "every new data plane has end-to-end regression coverage".
- **Needs observable traversal-rate data**: the success rate of symmetric NAT,
  port-restricted cone and other combinations must be measured across at least 10+ NAT
  combinations before deciding whether SO is worth being the default path.
- **Failure-latency cost**: SO's failure detection time (seconds) lengthens the
  direct→relay switch path and needs a dedicated timeout strategy and race architecture,
  not just appended code.

### 5. Acceptance criteria (the bar at implementation time)

1. under dual EIM NAT, direct establishes and the data path uses TLS-PSK encryption (as
   today);
2. under symmetric NAT, no NAT and single-sided NAT combinations, the relay fallback time
   is no worse than today;
3. ports bound for SO reusing the same local port are correctly released after server
   restart and generation change;
4. new e2e tests pass against a real NAT environment (or a controllable netns/iptables
   topology);
5. audit logs record the chosen path and failure reason, with no sensitive information
   other than addresses.

## Alternative routes (recorded, not adopted)

- **QUIC hole punching**: higher traversal rate, but introduces a full QUIC stack,
  conflicting with "minimal footprint".
- **Deploy a TURN relay**: server-side TURN relay covers symmetric NAT, but turns the
  server into a bandwidth aggregation point and changes the security and capacity model.
- **IPv6 first**: guiding users to IPv6 (no NAT) as a "zero-cost hole punch" can be
  recommended in the docs first; it does not preclude a future SO approach.
