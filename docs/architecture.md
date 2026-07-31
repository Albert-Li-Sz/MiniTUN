# Architecture

The target system consists of three Linux programs:

- `minitun`: a stateless, short-lived CLI that communicates only with the local daemon.
- `minitund`: the client daemon that owns persistence, credentials, server sessions,
  tunnel state, and local relay connections.
- `minitun-server`: the public TLS endpoint and remote TCP listener manager.

Each configured public server will have an isolated `ServerSession`, including its own
control connection, authentication state, heartbeat, reconnection controller, worker
pool, session generation, and tunnel registry. A failure in one session must not affect
another session.

## Stage-2 storage layer

The common layer provides the shared error/result model, structured logging, validated
endpoints and port ranges, typed random IDs, time helpers, and move-only secret
storage. Stage 2 adds `MiniTun::storage`, which is linked to `minitund` and the storage
tests. The CLI and public server do not link SQLite.

`StateRepository` owns one migrated `Database` and exposes `ServerRepository` and
`TunnelRepository`. Only the daemon opens or writes the state database. Stage-4 CLI
commands ask the daemon to perform every query and mutation over IPC.

### SQLite schema version 2

The default path is `/var/lib/minitun/state.db`. Version 2 contains:

- `schema_version`: `version` as the primary key and `applied_at` as a non-negative
  Unix-millisecond timestamp. The migration history must be non-empty, contiguous, and
  no newer than the version supported by the running binary.
- `servers`: `id`, unique nullable `name`, canonical `endpoint`, nullable
  `credential_ref`, nullable `remote_server_id`, desired and actual states, nullable
  last-error code and message, reconnect attempt, nullable latency, and creation/update
  timestamps.
- `tunnels`: `id`, nullable non-unique `name`, `server_id`, TCP protocol, split local
  and remote host/port pairs, desired and actual states, nullable last-error code and
  message, and creation/update timestamps.
- `daemon_identity`: one constrained row containing the stable `client_` identity used
  by all remote server sessions. It is created transactionally on first daemon start
  and retained across restarts.

IDs, text byte lengths, state values, TCP ports, counters, latency, and timestamps have
database constraints and are validated again when repository records cross the C++
boundary. Timestamps use Unix milliseconds. A tunnel references `servers(id)` with
`ON DELETE CASCADE`. Its remote-listener uniqueness key is
`(server_id, protocol, remote_host, remote_port)`, so different servers may use the same
remote endpoint. The reconciliation indexes are:

```text
idx_servers_reconcile(desired_state, id)
idx_tunnels_reconcile(server_id, desired_state, id)
idx_tunnels_name(name) WHERE name IS NOT NULL
```

The default in-process limits are 128 server records and 4096 tunnel records. Limits
are injectable for tests and future daemon configuration.

Every opened connection requires and verifies:

```sql
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;
PRAGMA wal_autocheckpoint = 1000;
PRAGMA journal_size_limit = 16777216;
```

WAL is enabled only after migration and schema validation succeed. Version-1 creation
runs in a `BEGIN IMMEDIATE` transaction. A newer version, a malformed migration
history, schema-definition drift, unexpected user schema objects, an integrity or
foreign-key violation, or a non-empty database without `schema_version` is rejected.
Failure rolls back the migration; MiniTun does not delete or rebuild the user's
database.

### Repositories and transactions

Both repositories provide validated create, lookup, deterministic list, update,
tombstone, and physical erase operations. Server names are unique. Tunnel names are
intentionally not unique, so a name lookup fails as ambiguous when it matches multiple
records. Marking a server removed also marks its child tunnels removed; physical
server deletion cascades only when the server and every child are removal tombstones.
Creation timestamps are immutable, and updates that would move a record timestamp
backward are rejected.

Each standalone mutation owns a transaction. Overloads accepting a shared
`Transaction` allow server and tunnel changes to commit atomically. Transactions are
connection-scoped and thread-affine, use `BEGIN IMMEDIATE`, reject nesting, keep the
connection lock for their lifetime, and roll back if abandoned or if a participating
repository operation fails. They must not span network or asynchronous work.

### Restart-state recovery

`StateRepository::recover()` performs normalization and snapshot loading in one
transaction:

| Persisted desired state | Credential reference | Recovered actual state |
| --- | --- | --- |
| Server `enabled` | absent | `not_authenticated` |
| Server `enabled` | present | `disconnected` |
| Server `disabled` or `removed` | either | `disabled` |
| Tunnel `active` | not applicable | `pending` |
| Tunnel `disabled` | not applicable | `disabled` |
| Tunnel `removed` | not applicable | `removing` |

Server reconnect attempts reset to zero and latency resets to null. A removed server
propagates the `removed`/`removing` tombstone state to every child tunnel. The operation
is idempotent and returns the fully validated server/tunnel snapshot only after all
updates succeed.

`minitund` calls recovery before accepting IPC. It then verifies every live
`credential_ref` against the separate credential store. A missing secret clears the
reference and restores `not_authenticated`; removed-server credentials are deleted.

## Stage-3 local IPC

`MiniTun::ipc` is independent of SQLite. The CLI links the common and IPC libraries;
only the daemon links storage. Each wire message has this bounded form:

```text
uint32 network-order JSON byte length | UTF-8 JSON payload
```

Requests and responses use protocol version 1 and a canonical `req_` identifier. The
request schema requires exactly `version`, `request_id`, `method`, and object-valued
`params`. A response carries either an object-valued `result` or a stable common error
code and non-sensitive message. Unknown top-level fields, malformed UTF-8, unsupported
versions, invalid IDs, non-object parameters, and messages over 1 MiB are rejected
before dispatch.

The dispatcher has a thread-safe method registry. Handler failures become protocol
error responses, while uncaught handler exceptions are contained and converted to a
generic `internal_error` without exposing exception text. Stage 4 registers all local
control methods through one `ControlService`.

`LocalServer` accepts multiple Unix-domain-socket sessions, bounds their total number,
and serves one request per connection. Each session incrementally decodes input and owns
its own failure boundary, so a malformed client cannot terminate the accept loop or
another session. Handlers run on a bounded four-thread dispatcher pool; an absolute
per-request deadline remains armed from the first read through the response write, so a
slow handler cannot block socket I/O or keep its session alive indefinitely. Pool
shutdown first closes submissions and then joins accepted work. `LocalClient`
separately bounds its connect and request phases.

The server creates the socket with mode `0660`; its owner must be the daemon's effective
user, while an authorized group may be configured. It requires a real daemon-owned
parent, rejects symbolic links throughout the directory chain, and permits a writable
ancestor only when sticky-directory semantics protect entries. A daemon-owned `0600`
sidecar lock serializes stale replacement and remains held through socket cleanup.
Startup refuses symlink and non-socket collisions, probes before replacing an owned
stale socket, and only removes the same socket inode it created. The production path is
`/run/minitun/minitun.sock`; packaging will later create its protected
`minitun:minitun` runtime directory and run the service as that account.

## Stage-4 local control plane

`MiniTun::daemon` owns the IPC control handlers and depends on both storage and IPC.
The CLI continues to link only common/IPC code and therefore cannot bypass daemon
authorization or database lifecycle rules. The registered methods are:

```text
daemon.status
status
server.add  server.login  server.list  server.inspect  server.remove
tun.add     tun.list      tun.inspect  tun.remove
```

Control handlers strictly reject missing, mistyped, out-of-range, and unknown
parameters. Compound reads and tunnel creation use a shared state transaction, so
concurrent CLI requests see a consistent server/tunnel relationship. Removal uses the
repository tombstone APIs; public list/inspect results filter those tombstones. A
tunnel is persisted as `active/pending` even when its server is disconnected.

The credential backend is a separate SQLite database at
`/var/lib/minitun/credentials.db`. It stores opaque key/blob pairs behind the
`CredentialStore` interface, enforces a daemon-owned regular file with mode `0600`,
uses bound parameters and transactional updates, enables SQLite secure deletion, and
refuses unsupported or unversioned non-empty schemas. The state database stores only
the opaque key. Tokens do not appear in responses, errors, or logs.

Token-bearing IPC buffers are proactively cleansed after serialization, transport,
parsing, and dispatch. `SecureString` remains the move-only representation used at the
credential boundary. File permissions and cleansing reduce exposure but do not turn
the credential database into an encrypted vault; filesystem and host trust still
matter.

The remote protocol library provides explicit 24-byte network-order headers, a 64 KiB
frame limit, incremental decoding, bounded payload fields, and separate control/worker
connection state machines. The public server accepts TLS 1.2-or-newer control
connections, performs challenge HMAC authentication, assigns a new generation to each
authenticated session, and enforces heartbeat deadlines.

## Stage-7 multi-server sessions

`ServerManager` periodically reconciles enabled, credentialed server records and owns
one independent `ServerSession` per server ID. Each session has its own strand,
resolver, TLS stream, operation timers, heartbeat state, session generation, and
jittered exponential reconnect controller. Authentication failure parks only that
server in `not_authenticated`; network and heartbeat failure enter bounded backoff.
Changing credentials or removing a server replaces only the affected session.

The daemon loads one stable `client_id` from schema version 2 before starting remote
work. It supports a platform trust store or explicit `--tls-ca`, hostname verification
and SNI by default, and fixed `--io-threads` bounded to 1..16. A development-only
`--insecure-skip-verify` switch emits a prominent warning. Tunnel registration and
Worker Pools build on these isolated session lifetimes.

## Stage-8 tunnel reconciliation

Each online client session compares persisted tunnel desired state with its in-memory
registered set during heartbeat processing. Missing active tunnels transition through
`registering` to `active`; server policy or bind failures transition only that tunnel
to `failed` with a stable error code and are retried on later reconciliation. Removed
or disabled tunnels are unregistered idempotently. A disconnected session clears its
runtime set and changes active tunnels back to `pending`, allowing a fresh generation
to recreate every listener after server or daemon restart.

The public server owns a `TunnelRegistry` on its Asio strand. A listener key contains
the authenticated `client_id` and `tunnel_id`, while ownership also records the current
session generation. Numeric address parsing, `--allow-ports`, per-client counts, and OS
bind errors are checked before acknowledgment.

## Stage-9 Worker Pools

Every daemon `ServerSession` owns a separate client Worker Pool and shares only a
bounded global Worker budget. Workers connect and verify TLS independently, carry the
current `client_id` and `session_generation`, and are replenished after consumption or
disconnect. Server-side idle capacity is capped per session and globally; stale
generation cleanup closes the underlying TLS connection immediately. Public sockets
wait for a matching Worker for at most two seconds, while idle Workers expire after 60
seconds by default. The stage-9 client intentionally returns `local_connect_failed`
after `START_RELAY`; stage 10 adds local dialing and raw relay without changing pool
ownership.
