# Operations & Observability

## Admin endpoints

Both `minitund` and `minitun-server` provide an HTTP admin endpoint that is disabled by
default:

```text
GET  /healthz
HEAD /healthz
GET  /readyz
HEAD /readyz
GET  /metrics
```

Loopback listeners may run without authentication:

```bash
minitund --admin-listen 127.0.0.1:9091 ...
minitun-server --admin-listen 127.0.0.1:9090 ...
```

A non-loopback listener must also configure a private `--admin-token-file`, and requests
use `Authorization: Bearer <token>`. The admin endpoint itself does not provide TLS;
non-loopback mode is only for trusted management networks, or behind a reverse proxy with
TLS and access control enabled.

The HTTP implementation only accepts the listed methods and paths, and limits header/body
sizes, concurrent connections and timeouts. `HEAD` returns the same status and headers as
the corresponding `GET`, but no body.

Health and readiness mean different things:

- daemon health checks the state and credential databases; readiness also requires the IPC
  to be listening, but not every remote being online;
- server health checks the process, and server readiness requires TLS, client policy and
  the control listener to all be available.

## Metrics

`/metrics` outputs Prometheus text covering sessions, connections, tunnels, Workers, relay,
authentication, registration, ACL/quota rejections, errors, reconnects, bytes, TLS session
resumption, policy reloads, queues and registration latency. Labels only use the fixed set
`role`, `state`, `result`, `direction` and similar; client/tunnel IDs or names are never
used.

All counters reset to zero after a process restart. Persistent business state should be
queried via local IPC/SDK, never reconstructed from metric counters.

## Audit logging

The following events are recorded under the `server.audit` or `daemon.audit` component:

- policy reloads and their results;
- authentication successes and generic failures;
- tunnel registration, deregistration and management operations;
- ACL, connection, tunnel and Worker quota rejections;
- credential logout, enable/disable, update, apply and prune.

Logs never contain PSKs, certificate contents, private keys, authentication digests or user
traffic. Client and tunnel identifiers only appear in individual structured audit events,
never in metric labels.

## Common probes

```bash
curl --fail http://127.0.0.1:9090/healthz
curl --fail http://127.0.0.1:9090/readyz
curl --fail http://127.0.0.1:9090/metrics

minitun health
minitun readiness
minitun metrics
minitun doctor --json --checkpoint
```

Server policy and TLS material are reloaded via `SIGHUP`; the daemon's `SIGHUP` or
`minitun reload` rebuilds remote sessions. When updating a single server's endpoint, TLS
server name or credentials, only that server's session restarts; other servers and
established relays are unaffected.

## Backup and restore

Back up the state and credential databases together before a production upgrade:

```bash
install -d -m 0700 /var/backups/minitun/pre-v1
minitun doctor --json \
  --backup-state /var/backups/minitun/pre-v1/state.db \
  --backup-credentials /var/backups/minitun/pre-v1/credentials.db
```

Backups are paired; restore should also use the two files generated at the same point in
time.
