# Remote Protocol v2

MiniTun only accepts Remote Protocol v2; there is no compatibility fallback to an older
protocol. All remote messages travel inside TLS 1.2+; there is no plaintext fallback.
C/C++ object layouts are never put on the wire directly; all integers, lengths and fields
use explicit network byte order encoding.

## Frame format

Every control or Worker handshake message starts with a fixed 24-byte header:

| Offset | Width | Field | v2 value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | `0x4D54554E` (`MTUN`) |
| 4 | 2 | version | `2` |
| 6 | 2 | message type | the message types defined below |
| 8 | 4 | flags | must be `0` for now |
| 12 | 4 | payload length | unsigned byte count after the header |
| 16 | 8 | request ID | request/response correlation; zero only for messages that need no correlation |

A single frame is capped at 64 KiB. The decoder rejects over-long claims before allocating
payload, supports arbitrary TCP fragmentation and reading multiple frames at once, and
strictly rejects unknown types, non-zero reserved flags, malformed UTF-8, embedded NULs,
trailing bytes and illegal state transitions.

A client's registration sync window holds at most 32 unfinished requests. Frames within the
window are coalesced into one bounded TLS application write; responses may arrive out of
order and are correlated by request ID, tunnel ID and desired revision.

## Capability negotiation and authentication

Control handshake:

```text
client  -> HELLO(client_id, offered_capabilities)
server  -> HELLO_ACK(server_id, server_time, nonce, selected_capabilities)
client  -> AUTH(client_id, timestamp, nonce, selected_capabilities, HMAC)
server  -> AUTH_OK(session_generation, heartbeat_interval, worker_limits)
       or AUTH_ERROR(authentication_failed)
```

v2 defines the following capability bits:

| Capability | Bit value | Status and purpose |
| --- | --- | --- |
| `pipelined_control` | `1ULL << 0` | required |
| `per_client_policy` | `1ULL << 1` | required |
| `tunnel_revisions` | `1ULL << 2` | required |
| `client_certificate_binding` | `1ULL << 3` | supported by both, used per policy |
| `multiplexed_streams` | `1ULL << 4` | reserved, but not supported or advertised by default |
| `udp_datagrams` | `1ULL << 5` | supported by both, required for UDP mode |
| `socks5_proxy` | `1ULL << 6` | supported by both, required for SOCKS5 mode |
| `p2p_rendezvous` | `1ULL << 7` | supported by both, required for P2P mode |
| `tcp_simultaneous_open` | `1ULL << 8` | supports server-assisted P2P TCP hole punching; allows the public peer source endpoint in `START_RELAY` |
| `proxy_protocol` | `1ULL << 9` | allows the public source endpoint in `START_RELAY` for PROXY protocol v1 headers on TCP tunnels with `proxy_protocol` enabled |
| `worker_observed_endpoint` | `1ULL << 10` | appends the Worker's own server-observed IP after the source endpoint extension, letting the daemon advertise a NAT-reachable P2P candidate |

The server may only select the set the client offered and itself supports, and must include
the three required capabilities. The control authentication digest is HMAC-SHA256 keyed by
that client's PSK, bound to: protocol version, client ID, server ID, timestamp, 32-byte
random nonce and the final capability set. Neither the PSK nor the digest is ever logged in
plaintext.

The server also checks bounded clock skew, a nonce replay cache and source rate limiting,
and compares the digest in constant time. Unknown, disabled, certificate-mismatched or
wrong-PSK all get the same non-sensitive authentication failure; the implementation still
runs an equivalent HMAC path so error text or obvious early exits cannot reveal policy
existence.

A policy may additionally bind a client certificate SHA-256 fingerprint or SAN. In that
case the TLS chain must validate against `--client-ca`, and both the control connection and
its Workers must match the same client policy; a certificate cannot replace the PSK.

Every successful control authentication generates a new non-zero 64-bit
`session_generation`. Listeners, Workers or responses from an old generation must not
modify current state. Heartbeats use matched `PING/PONG` sequence numbers and advertise
Worker idle deadlines in a bounded bit range.

## Tunnel registration

```text
client -> REGISTER_TUNNEL(tunnel_id, bind_host, bind_port, desired_revision[, mode])
server -> REGISTER_TUNNEL_OK(tunnel_id, desired_revision)
       or REGISTER_TUNNEL_ERROR(tunnel_id, error_code, desired_revision)

client -> UNREGISTER_TUNNEL(tunnel_id, desired_revision)
server -> UNREGISTER_TUNNEL_OK(tunnel_id, desired_revision)
```

`local_host` and `local_port` never enter the remote protocol; the server only sees the
public binding and the transport mode. TCP keeps the original payload with no appended mode
byte; UDP, SOCKS5 and P2P each append an explicit mode byte and require the corresponding
capability to be negotiated for the current session. Before registering, the numeric bind
address, client ACL, per-client/global tunnel quota and OS bind result are checked. Both
the daemon and server require a numeric loopback for SOCKS5 binds. Before creating a
listener, the server rejects wildcard addresses, non-loopback IPs and hostnames, including
registrations sent directly by clients that bypass the daemon.

The daemon commits a response only when session generation, frame request ID, tunnel ID and
the current `config_revision` all match. Duplicate and stale responses are ignored. A
timeout or disconnect converges any residual `registering`, `removing`, `active` state of
that generation to `pending`.

When changing the public port or address, the daemon first sends an unregister for the old
listener and only registers the new desired revision after receiving the corresponding
acknowledgement. If registering the new port fails, the new desired configuration is kept
and marked `failed`, and the old entry is not restored.

## Workers and relay

Workers use a separate TLS connection:

```text
client -> WORKER_HELLO(client_id, generation, worker_id, timestamp, nonce, HMAC)
server -> WORKER_ACCEPTED(worker_id)

server -> REQUEST_WORKERS(count)                 # control connection
server -> START_RELAY(tunnel_id, connection_id[, mode]) # allocated Worker
client -> LOCAL_CONNECT_OK(connection_id)
       or LOCAL_CONNECT_ERROR(connection_id, local_connect_failed)
```

The Worker HMAC binds client ID, server ID, generation, worker ID, timestamp and nonce in
the protocol-version context. The server requires the current control generation, the same
policy, the same PSK and the same certificate binding.

A public connection consumes per-client and global connection quota before waiting for a
Worker. Idle Worker counts are constrained by the client policy, the server cap and the
daemon cap simultaneously, and are replenished adaptively between minimum and maximum on
demand.

Without endpoint extensions, TCP `START_RELAY` keeps the existing wire image; non-TCP
modes append the same enum byte. Workers inherit their control session's capability set.
After negotiating `proxy_protocol` or `tcp_simultaneous_open`, the server may append the
public connection's `source_host` and `source_port`. This extension always starts with an
explicit mode byte, including for TCP. Both fields must be present together: the host is
a numeric IP and the port is a non-zero `uint16`. Only when the source endpoint extension
is present and `worker_observed_endpoint` is also negotiated does the server append
`worker_observed_host` (the Worker's TLS connection IP as observed by the server, with no
port). Older peers that did not negotiate extensions still receive the original encoding.

A source endpoint alone does not enable PROXY headers. The daemon only prepends a PROXY
protocol v1 header to the target connection when the TCP tunnel's local `proxy_protocol`
configuration is `true`.

After receiving `LOCAL_CONNECT_OK`, that Worker permanently leaves Remote Protocol frame
mode and carries a single data plane:

| mode | Worker data plane |
| --- | --- |
| `tcp` | raw TLS application bytes; the daemon connects a fixed local TCP target. |
| `udp` | `uint16` big-endian payload length + 0..65,507 bytes of payload; each record preserves one UDP datagram boundary. |
| `socks5` | SOCKS5 no-auth CONNECT handshake inside TLS, supporting IPv4/IPv6/domain; raw TCP bytes after success. |
| `p2p` | one-time token and direct candidate negotiation; on success switches to TLS 1.3-encrypted direct TCP, otherwise confirms and continues the TLS relay. |

TCP relay uses a fixed 16 KiB buffer per direction and only reads after finishing writing
the current block, forming bounded backpressure. EOF propagates as a half-close, and data in
the reverse direction may continue; an idle timeout or reset closes the relay and releases
the quota exactly once. UDP public peer sessions, queued datagram counts and total bytes are
also bounded and released automatically when idle.

P2P candidates authenticate with a random one-time token bound to a single negotiation.
After candidate authentication, both ends upgrade the direct socket to TLS 1.3, using the
token as an external PSK to encrypt application data. Direct contexts apply the shared
explicit cipher policy and compression/renegotiation restrictions, allowing only TLS 1.3.
When `tcp_simultaneous_open` is negotiated and enabled by the connector, a failed ordinary
direct connection can trigger server-assisted TCP hole punching, supporting dual EIM NAT.
ICE, STUN, TURN and UDP hole punching are not implemented. Failed connections or
confirmations automatically fall back to the original authenticated TLS Worker.

## Reload and shutdown

The server atomically switches only after fully validating the new TLS/policy snapshot. A
client that is deleted, disabled or has an effective policy change:

1. immediately revokes its listener and stops accepting new relays;
2. closes the control connection and idle Workers;
3. lets allocated relays drain for at most `--shutdown-timeout`, then forcibly closes them.

Unchanged clients' sessions do not churn on a pure policy reload. A pure server TLS
credential reload only affects new connections and the rotation of control/idle Workers,
without interrupting in-flight relays.

`GOAWAY` may be sent by either authenticated control peer. On process exit, listeners and
new allocations stop first, then relays drain within the same grace period; on failure the
peer must tolerate the transport closing directly.

## Compatibility boundary

- All 1.x versions only accept protocol number 2; there is no downgrade or lenient parsing
  of v1.
- TCP REGISTER retains its original encoding; without negotiated endpoint extensions,
  TCP START payloads are identical to 1.0. New modes and endpoint metadata are appended
  only after both sides negotiate the corresponding capabilities, so a 1.0 peer never
  receives an unparseable non-TCP tunnel or endpoint extension.
- It still keeps "one relay per one TLS Worker" and does not implement
  `multiplexed_streams`.
- `libminitun-remote-protocol.so.1` exposes the strongly-typed message variant shared with
  the wire codec, the incremental frame decoder, frame/message codec and control/Worker
  authentication digest helpers; it does not create sockets, TLS sessions, daemon or server
  runtimes.
