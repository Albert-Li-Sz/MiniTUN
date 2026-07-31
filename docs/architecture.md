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
`TunnelRepository`. The production ownership model allows only the daemon to write the
state database; later IPC-based CLI commands will ask the daemon to perform changes
instead of opening SQLite directly.

### SQLite schema version 1

The default path is `/var/lib/minitun/state.db`. Version 1 contains:

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

The recovery API is ready for daemon startup, but the current `minitund` entry point
does not call it. Stage 3 will add local IPC; later stages add the CLI workflow,
credential backend, remote protocol, isolated server sessions, TLS, reconciliation,
worker pools, and TCP relay.
