# Configuration & Client Policies

MiniTun v1 has two strict JSON files: the client policy on the public server, and the
declarative resource configuration for each `minitund`. Both reject duplicate keys, unknown
fields, wrong types and values above limits; the current configuration is only switched
after the whole file validates successfully.

## Server client policy

`minitun-server` must be given a policy file via `--clients-config`. Each client has a
stable `client_id` and its own PSK; the port ACL, tunnel, connection and idle Worker quotas
are also configured per client.

```json
{
  "format_version": 1,
  "clients": [
    {
      "client_id": "client_0123456789abcdef0123456789abcdef",
      "enabled": true,
      "psk_file": "/etc/minitun-server/clients/team-a.psk",
      "allowed_ports": ["6000-6099", "8443"],
      "max_tunnels": 100,
      "max_connections": 1000,
      "max_idle_workers": 32,
      "allowed_source_cidrs": ["203.0.113.0/24", "2001:db8::/32"],
      "connections_per_minute": 60,
      "certificate_san": "URI:spiffe://example.internal/minitun/team-a"
    }
  ]
}
```

`certificate_san` and `certificate_sha256` are mutually exclusive; both may also be
omitted. When either certificate binding is enabled:

- the server must be configured with `--client-ca`;
- both the control connection and Workers must present a certificate validated by that CA;
- the certificate must also match the SAN or lowercase hex SHA-256 fingerprint in the
  policy;
- the PSK is still required; a certificate cannot replace the PSK.

`allowed_ports` is an array of closed intervals and only restricts the public listening
ports. Ranges must not overlap; rejections record a bounded audit event but never turn
client or tunnel names into metric labels.

Two optional fields control source admission for public ports:

- `allowed_source_cidrs`: a CIDR whitelist (IPv4/IPv6, 1–64 entries). When present, only
  public connections from these networks are accepted; empty (the default) allows all
  sources.
- `connections_per_minute`: per-source-IP connection rate limit (per minute, max
  1000000). The default `0` means no rate limit.

Connections rejected by source policy count toward the
`minitun_source_rejections_total` metric.

The PSK file must be a regular file owned by the current service account and inaccessible
to group and other users. Trailing CR/LF is normalized on read. Certificate and policy
files may be group-readable but must not be writable by group or other users.

Reloading the policy safely:

```bash
sudo install -m 0640 -o minitun-server -g minitun-server \
  clients.json.new /etc/minitun-server/clients.json.new
sudo mv /etc/minitun-server/clients.json.new /etc/minitun-server/clients.json
sudo systemctl kill -s HUP minitun-server.service
```

On parse or validation failure the old snapshot is kept. Clients that are disabled, deleted
or whose credentials changed stop receiving new traffic; active relays drain within a grace
period, then the control connection and idle Workers disconnect. Unchanged clients do not
churn.

## TLS admission protection

The public TLS listener applies admission protection before allocating TLS objects. Control
connections and Workers share these budgets:

| Server option | Default | Behavior |
| --- | --- | --- |
| `--max-pending-handshakes` | 128 | Global unauthenticated connection limit, range 1–4096. |
| `--max-pending-handshakes-per-ip` | 32 | Unauthenticated connections per source IP; cannot exceed the global limit. |
| `--max-handshakes-per-second` | 100 | Listener accepts per second, range 1–100000; the token bucket holds one second of burst capacity, and rejected TCP connections also consume tokens. |
| `--handshake-timeout` | 10 seconds | Total time from admission through TLS and application authentication, range 1–300 seconds. Completing TLS does not reset this deadline or release the pending quota. |

When the global pending or rate budget is exhausted, the listener waits on a timer to avoid
an accept/close CPU loop. Authenticated control connections and Workers release their pending
reservations and continue under the existing session/relay quotas. Five TLS handshake failures
from one IP in a minute block that IP for a minute. IPv4 and its IPv6 mapped representation
share the same source quota. The failure cache holds at most 4096 sources. TLS warnings are
limited to one record every five seconds across all sources; metrics retain all failures.

These are startup options and require a server restart. Adjust the per-IP limit and rate for
clients sharing a NAT address or creating Workers in batches. Client policy
`connections_per_minute` applies to public tunnel ports; the TLS listener budget is independent.

## Memory budget and connection limits

Every relay reserves two fixed 16 KiB data-plane buffers, so `--max-total-connections`
directly sets the relay memory floor: the default 50000 implies roughly 1.6 GiB, well above
the `MemoryMax=512M` in `packaging/systemd/minitun-server.service`. At startup the server
compares its configured ceiling against the visible memory ceiling (cgroup v2/v1 limit
first, then `RLIMIT_AS`, then `RLIMIT_DATA`) and logs one `resource_exhausted` warning when
the estimate exceeds it, naming the estimate, the connection limit, and the memory limit.

The warning never blocks startup. Pick one of the two options for the deployment:

```bash
# Tighten the connection limit to fit a ~512 MiB budget
minitun-server --max-total-connections 12000 ...

# Or raise the service memory ceiling with a drop-in
sudo systemctl edit minitun-server.service   # add [Service] MemoryMax=2G
```

With no visible memory ceiling (bare process, no rlimit) the warning is not emitted.

## Declarative resource configuration

The local configuration uses `format_version: 1` and contains `servers` and `tunnels`:

```json
{
  "format_version": 1,
  "servers": [
    {
      "name": "edge",
      "endpoint": "tunnel.example.com:2333",
      "tls_server_name": "tunnel.example.com",
      "psk_file": "secrets/edge.psk",
      "ca_file": "secrets/organization-ca.pem",
      "client_cert_file": "secrets/client-chain.pem",
      "client_key_file": "secrets/client-key.pem",
      "enabled": true
    }
  ],
  "tunnels": [
    {
      "name": "web",
      "server": "edge",
      "protocol": "tcp",
      "local_host": "127.0.0.1",
      "local_port": 8080,
      "remote_host": "0.0.0.0",
      "remote_port": 6000,
      "enabled": true
    },
    {
      "name": "dns-udp",
      "server": "edge",
      "protocol": "udp",
      "local_host": "127.0.0.1",
      "local_port": 5353,
      "remote_port": 6001,
      "enabled": true
    },
    {
      "name": "private-proxy",
      "server": "edge",
      "protocol": "socks5",
      "remote_host": "127.0.0.1",
      "remote_port": 6002,
      "enabled": true
    },
    {
      "name": "p2p-web",
      "server": "edge",
      "protocol": "p2p",
      "local_host": "127.0.0.1",
      "local_port": 8080,
      "remote_port": 6003,
      "enabled": true
    }
  ]
}
```

Tunnel field rules:

| Field | Rule |
| --- | --- |
| `protocol` | optional, default `tcp`; may be `tcp`, `udp`, `socks5`, `p2p`. |
| `local_host` | optional, default `127.0.0.1`; ignored in SOCKS5 mode. |
| `local_port` | required for TCP, UDP, P2P; optional for SOCKS5. |
| `remote_host` | optional; default `0.0.0.0` for TCP/UDP/P2P, default and only allowed as a numeric loopback for SOCKS5. |
| `remote_port` | required, range 1..65535, and constrained by the server's `allowed_ports`. |
| `proxy_protocol` | optional boolean, default `false`; only `tcp` may enable it to prepend a PROXY protocol v1 header to the local target connection. |

SOCKS5 only implements no-auth CONNECT; both the daemon and server enforce a numeric
loopback bind. P2P first tries LANs or already-routable paths, then attempts negotiated
server-assisted TCP simultaneous open, supporting dual-EIM-NAT traversal. It does not
include ICE/STUN/TURN or UDP hole punching. After one-time token authentication, the direct
path upgrades to TLS 1.3 using that token as an external PSK to encrypt application data;
a failed direct connection automatically falls back to the TLS relay.

Relative credential paths are resolved against the directory containing the config file.
`plan` is fully read-only and actions are sorted by resource type and stable key:

```bash
minitun config plan /etc/minitun/config.json
minitun config apply /etc/minitun/config.json
```

Matching rules: by stable ID first; when there is no ID, servers match by unique name and
tunnels by unique name within the same type. An existing tunnel's ID and server ownership
cannot change. Re-applying the same file returns zero actions and does not rebuild remote
sessions.

By default apply only creates and updates. An explicit `--prune` deletes resources
previously managed by apply but missing this time; imperatively created resources are never
pruned:

```bash
minitun config plan /etc/minitun/config.json --prune
minitun config apply /etc/minitun/config.json --prune
```

apply first parses all resources and credentials fully, validates TLS material, stages the
new secrets, then switches resources and credential references in a single state
transaction. A failure cleans up staged items; the daemon also cleans up unreachable
credentials left behind by a crash at startup.

`config export` contains no paths or secrets, only boolean flags for whether credentials
are configured. The exported flags can be used for review; when re-applying without the
corresponding `*_file`, the current credentials are preserved.
