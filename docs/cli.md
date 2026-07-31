# Command-line interface

`minitun` is a stateless, short-lived client. It never opens SQLite and never connects
to a public MiniTun server; every resource command sends one request to the local
`minitund` Unix socket and exits after printing the response.

## Commands

```text
minitun server add <server-endpoint> [--name <name>]
minitun server login <server-id-or-name> [--token-stdin]
minitun server list [--json]
minitun server inspect <server-id-or-name> [--json]
minitun server remove <server-id-or-name>

minitun tun add <server-id-or-name> <local-port> <server-port>
                [--local-host <host>] [--name <name>]
minitun tun list [server-id-or-name] [--json]
minitun tun inspect <tun-id-or-name> [--json]
minitun tun remove <tun-id-or-name>

minitun status
minitun daemon status
minitun version
minitun help
```

All commands accept the global override:

```text
--socket <path>   Local minitund socket (default /run/minitun/minitun.sock)
```

`list --json` prints a JSON array. `inspect --json` prints one JSON object. Server JSON
contains only `credential_configured`; it never exposes the credential reference or
Token. Removed tombstones are not returned by list or inspect commands.

## Token input

Without `--token-stdin`, `server login` requires an interactive terminal and disables
echo while reading one Token line:

```text
Token:
```

For automation, opt in to reading one line from standard input:

```bash
printf '%s\n' "$MINITUN_TOKEN" |
  minitun server login primary --token-stdin
```

A Token is not accepted as a positional argument or regular option, keeping it out of
shell history, process arguments, and `/proc`. Stage 4 stores the credential locally
and reports the server as `disconnected`; remote authentication begins in stage 6.

## Tunnel semantics

The default command:

```bash
minitun tun add primary 22 6000
```

persists this desired TCP route:

```text
public server 0.0.0.0:6000 -> local client 127.0.0.1:22
```

The record is created as `desired_state=active` and `actual_state=pending` even while
the public server is offline. A custom local target and display name are supported:

```bash
minitun tun add primary 8080 6001 \
  --local-host 192.168.1.10 \
  --name nas-web
```

## Exit codes

| Code | Meaning |
| ---: | --- |
| `0` | Success |
| `2` | Invalid arguments, unknown resource, or conflicting resource |
| `3` | Local daemon unavailable or inaccessible |
| `4` | Authentication failure |
| `5` | Remote or network failure |
| `10` | Protocol, database, resource, or internal failure |

CLI11 help/version control flow also exits with `0`; all other parse failures are
normalized to `2`.

## Daemon options

```text
minitund [--foreground]
          [--socket /run/minitun/minitun.sock]
          [--database /var/lib/minitun/state.db]
          [--credentials /var/lib/minitun/credentials.db]
```

`minitund` opens both databases, migrates them, normalizes restart state, validates
that every persisted credential reference exists, then starts IPC. Parent directories
must already exist and be owned/protected for the daemon account.
