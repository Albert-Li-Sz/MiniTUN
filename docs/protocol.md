# Remote protocol

MiniTun uses a TLS-protected binary protocol. C++ object layouts are never sent on
the wire; every integer and field is encoded explicitly in network byte order.

## Frame format

Every framed message starts with this fixed 24-byte header:

| Offset | Width | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | `0x4D54554E` (`MTUN`) |
| 4 | 2 | version | `1` |
| 6 | 2 | message type | one of the values below |
| 8 | 4 | flags | `0` in protocol version 1 |
| 12 | 4 | payload length | unsigned bytes following the header |
| 16 | 8 | request ID | request/response correlation identifier |

The complete header plus payload is limited to 64 KiB. A decoder rejects the
length before allocating payload storage when the declared frame is larger than
that limit. Empty payloads are valid. Incremental decoding supports arbitrary TCP
fragmentation and multiple frames in one read.

## Message types

Control connections use `HELLO`, `HELLO_ACK`, `AUTH`, `AUTH_OK`, `AUTH_ERROR`,
`REGISTER_TUNNEL`, `REGISTER_TUNNEL_OK`, `REGISTER_TUNNEL_ERROR`,
`UNREGISTER_TUNNEL`, `UNREGISTER_TUNNEL_OK`, `REQUEST_WORKERS`, `PING`, `PONG`,
`GOAWAY`, and `ERROR`.

Worker connections use `WORKER_HELLO`, `WORKER_ACCEPTED`, `START_RELAY`,
`LOCAL_CONNECT_OK`, and `LOCAL_CONNECT_ERROR`. After `LOCAL_CONNECT_OK`, the
connection permanently leaves framed mode and carries raw TCP bytes until close.

## Payload fields

Payload integers use network byte order. Byte strings and UTF-8 strings use a
two-byte length followed by exactly that many bytes. Strings reject malformed
UTF-8 and embedded NUL bytes. Each decoder applies a field-specific maximum in
addition to the frame-wide maximum.

## State validation

Client and server state machines independently validate control and worker
handshakes. Handshake messages cannot be replayed after authentication, control
messages cannot appear on workers, worker messages cannot appear on control
connections, and no framed message is accepted after the raw-relay transition.
Protocol violations close only the offending connection.

## TLS and authentication

All remote frames are carried inside TLS. The server requires TLS 1.2 or newer,
disables compression and renegotiation, loads a matching PEM certificate/private-key
pair, and never offers a plaintext fallback. Clients use SNI and hostname verification
against either the configured CA bundle or the platform trust store.

The control handshake is:

```text
client  -> HELLO(client_id)
server  -> HELLO_ACK(server_id, server_time, 32-byte nonce)
client  -> AUTH(client_id, timestamp, nonce, authentication_data)
server  -> AUTH_OK(session_generation, heartbeat and worker limits)
       or AUTH_ERROR(authentication_failed)
```

`authentication_data` is HMAC-SHA256 over the protocol version, length-prefixed
client ID, signed Unix timestamp encoded as 64 network-order bits, and the
length-prefixed server nonce. The Token is the HMAC key and is never sent. The server
checks clock skew, consumes every challenge nonce through a bounded replay cache,
compares HMAC output in constant time, and throttles failures by peer address. Failure
responses do not reveal whether the ID, timestamp, nonce, or digest was wrong.

Every successful authentication replaces the client's previous random 64-bit
`session_generation`; generation zero is invalid. The server then sends numbered
`PING` messages and requires matching `PONG` responses before the bounded heartbeat
deadline.

## Tunnel registration

An authenticated control connection reconciles each locally persisted active tunnel:

```text
client -> REGISTER_TUNNEL(tunnel_id, bind_host, bind_port)
server -> REGISTER_TUNNEL_OK(tunnel_id)
       or REGISTER_TUNNEL_ERROR(tunnel_id, error_code)

client -> UNREGISTER_TUNNEL(tunnel_id)
server -> UNREGISTER_TUNNEL_OK(tunnel_id)
```

The client never sends `local_host` or `local_port`; those remain local-only persisted
configuration. The server accepts numeric public bind addresses only, applies its
`--allow-ports` range before binding, limits tunnels per authenticated client, and maps
OS address conflicts to `remote_port_in_use`. Request IDs correlate every response.
Registration and removal are idempotent, and all listeners belong to one client
session generation so stale control sessions cannot retain them.

## Worker pool

After authentication, `minitund` preconnects the minimum number of TLS Workers for
that server session. A Worker identifies itself with `client_id`, the current
`session_generation`, and a random Worker ID. The server accepts only the generation
currently owned by the authenticated control connection:

```text
client -> WORKER_HELLO(client_id, session_generation, worker_id)
server -> WORKER_ACCEPTED(worker_id)

server -> REQUEST_WORKERS(count)  # on the control connection when capacity is low
```

Idle Workers are bounded per session and globally, expire after 60 seconds by default,
and are removed immediately when their control generation closes. A public connection
waits at most two seconds for a matching Worker. Assignment sends
`START_RELAY(tunnel_id, connection_id)`; the local target never crosses the wire.
