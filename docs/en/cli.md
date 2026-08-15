# Command Line Interface

`minitun` is a stateless, short-lived client. It does not open SQLite and does not connect
to the public server directly; every command only sends one IPC envelope v1 request to the
local `minitund` Unix socket.

Global option:

```text
--socket <path>   daemon socket, default /run/minitun/minitun.sock
```

## daemon and runtime state

```text
minitun daemon status
minitun daemon identity [--json]
minitun status [--json]
minitun health
minitun readiness
minitun metrics
minitun reload
minitun version
```

`daemon identity` returns the `client_id`, which is stable across restarts and is used in
the server's `clients.json`. Daemon readiness checks the state database, credential
database and IPC; it does not require every remote to be online. `metrics` counters reset
to zero after a process restart.

## server lifecycle

```text
minitun server add <endpoint> [--name <name>]
minitun server login <id-or-name> [--psk-stdin]
minitun server update <id-or-name>
        [--name <name> | --clear-name]
        [--endpoint <host:port>]
        [--tls-server-name <name> | --clear-tls-server-name]
        [--ca-file <pem> | --clear-ca]
        [--client-cert <pem> --client-key <pem> | --clear-client-identity]
minitun server enable <id-or-name>
minitun server disable <id-or-name>
minitun server logout <id-or-name>
minitun server list [--json]
minitun server inspect <id-or-name> [--json]
minitun server remove <id-or-name>
```

`server login` shows a `PSK:` prompt with echo disabled on a TTY by default. Automation
must explicitly read from standard input:

```bash
minitun server login edge --psk-stdin </secure/path/edge.psk
```

The PSK is supplied via `--psk-stdin` or no-echo TTY input, never as a positional argument
or ordinary option, so it does not enter shell history, process arguments or `/proc`. JSON
responses only return boolean status such as `credential_configured`, never secrets or
credential references.

Changes to endpoint, TLS server name, CA or client certificate identity only rebuild the
corresponding server session. A name change does not churn the session. `disable` keeps the
resource record; `enable` automatically resumes synchronization. `logout` deletes that
server's PSK, CA, client cert and private key and switches it to the unauthenticated state.

## tunnel lifecycle

```text
minitun tun add <server-id-or-name> <local-port> <server-port>
        [--local-host <host>] [--remote-host <numeric-host>]
        [--protocol tcp|udp|socks5|p2p] [--name <name>]
minitun tun update <tun-id-or-name>
        [--name <name> | --clear-name]
        [--local-host <host>] [--local-port <port>]
        [--remote-host <numeric-host>] [--server-port <port>]
        [--protocol tcp|udp|socks5|p2p]
minitun tun enable <tun-id-or-name>
minitun tun disable <tun-id-or-name>
minitun tun list [server-id-or-name] [--json]
minitun tun inspect <tun-id-or-name> [--json]
minitun tun remove <tun-id-or-name>
```

Example:

```bash
minitun tun add edge 8080 6000 --name web
```

means `public server 0.0.0.0:6000/tcp -> daemon 127.0.0.1:8080/tcp`. Even if the server is
offline, the record is created with `desired_state=active`, `actual_state=pending`. The
stable tunnel ID and server ownership cannot be updated; the name, local address/port and
public port can.

Creation examples for the four modes:

```bash
# public UDP 6001 -> local UDP 5353
minitun tun add edge 5353 6001 --protocol udp --name dns

# SOCKS5 CONNECT on the server loopback; local-port=1 is only a compatibility positional
minitun tun add edge 1 6002 --protocol socks5 \
  --remote-host 127.0.0.1 --name proxy

# fixed local TCP target for a P2P tunnel
minitun tun add edge 8080 6003 --protocol p2p --name direct-web
minitun-p2p tunnel.example.com:6003 --listen 127.0.0.1:6501
```

`--remote-host` is the numeric bind address on the server side, default `0.0.0.0`. SOCKS5
enforces a loopback bind and only supports no-auth `CONNECT` (IPv4, IPv6, domain); it does
not support BIND or UDP ASSOCIATE. UDP preserves datagram boundaries, with a single payload
limit of 65,507 bytes. P2P first tries a direct TCP candidate authenticated by a one-time
token, and automatically falls back to the TLS relay on failure; it provides no
ICE/STUN/TURN/NAT hole punching; the direct path is encrypted via TLS 1.3 PSK.

When changing the public port, the old listener is revoked before the new port is
registered. If the new port fails, the resource keeps the new desired configuration and
shows `failed`, and the old entry does not remain. `disable` keeps the record and
unregisters the listener; `enable` converges again afterwards. `remove` deletes the record
and asynchronously guarantees the remote listener no longer forwards.

Key diagnostic fields in tunnel JSON:

| Field | Meaning |
| --- | --- |
| `config_revision` | monotonically increasing desired configuration revision |
| `server_actual_state` | current actual state of the owning server |
| `pending_reason` | stable reason for `pending`, otherwise `null` |
| `last_synced_at` | Unix-millisecond timestamp of the most recent matching remote response |
| `last_error` | most recent stable, non-sensitive sync error |

## Declarative configuration

```text
minitun config export
minitun config plan <format-version-1.json> [--prune]
minitun config apply <format-version-1.json> [--prune]
```

plan is read-only and sorts create/update/disable/delete actions stably. apply does not
delete by default; only `--prune` deletes resources previously managed by apply but now
missing, while imperative resources are unaffected. Full pre-validation and a state
transaction guarantee a failure never leaves half a resource set; re-applying the same
config returns zero actions and does not rebuild sessions.

`config export` does not output credential paths or contents, only whether they are
configured. See the [Configuration documentation](/en/configuration) for the complete
format.

## Database diagnostics, backup and restore

```text
minitun doctor [--json] [--checkpoint]
       [--backup-state <path>] [--backup-credentials <path>]
       [--restore-state <path>] [--restore-credentials <path>]
```

Before a production upgrade, generate same-batch state/credentials backups while the
daemon is online:

```bash
install -d -m 0700 /var/backups/minitun/pre-v1
minitun doctor --json --checkpoint \
  --backup-state /var/backups/minitun/pre-v1/state.db \
  --backup-credentials /var/backups/minitun/pre-v1/credentials.db
```

An online restore verifies both sources first, then atomically replaces each and wakes
synchronization; the two SQLite files are not in the same database transaction, so paired
backups are mandatory. Downgrading to an older schema requires an offline restore of the
paired backups.

## P2P connector

```text
minitun-p2p <server-host:tunnel-port>
  [--listen <numeric-host:port>] [--relay-only]
  [--simultaneous-open|--no-simultaneous-open]
  [--connect-timeout <seconds>] [--negotiation-timeout <seconds>]
  [--direct-timeout <seconds>] [--inactivity-timeout <seconds>]
  [--allow-non-loopback]
```

It listens on `127.0.0.1:6501` by default. Each local connection negotiates independently
and prints the selected `direct` or `relay` path; `--relay-only` is useful for policy
control and verifying fallback. Only use `--allow-non-loopback` when you explicitly want to
expose the local entry to a trusted network.

`--simultaneous-open` (default on) requests a TCP simultaneous open after a failed direct
candidate: the daemon and the connector each connect to the other's server-observed
endpoint from the same local port, punching NATs with endpoint-independent mappings on
both ends; failure still falls back to the relay. This path requires both sides to be
v1.1+; use `--no-simultaneous-open` when pairing with an older daemon.

## Output and exit codes

`status --json`, `doctor --json`, health/readiness/metrics/reload and config commands
output a JSON object; `list --json` outputs an array; `inspect --json` outputs a single
object. Tombstones and internal credential references never appear in public results.

| Code | Meaning |
| ---: | --- |
| `0` | success |
| `2` | invalid argument, unknown resource or conflict |
| `3` | local daemon unavailable or unreachable |
| `4` | authentication failure |
| `5` | remote, TLS or network failure |
| `10` | protocol, database, resource exhaustion or internal failure |

## key daemon options

```text
minitund [--foreground]
  [--socket /run/minitun/minitun.sock]
  [--database /var/lib/minitun/state.db]
  [--credentials /var/lib/minitun/credentials.db]
  [--admin-listen <numeric-host:port>] [--admin-token-file <file>]
  [--tls-ca <pem>] [--relay-idle-timeout <seconds>]
  [--shutdown-timeout <seconds>]
  [--max-idle-workers-per-server <n>] [--max-total-idle-workers <n>]
  [--max-total-connections <n>] [--io-threads <1..16>]
```

`--insecure-skip-verify` is for local development only and must not be used in production.
