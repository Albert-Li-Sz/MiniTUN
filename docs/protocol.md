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
