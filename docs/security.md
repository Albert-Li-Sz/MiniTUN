# Security design status

MiniTun provides a protected local control plane, authenticated TLS control sessions,
generation-scoped TLS Worker Pools, a bounded raw TCP relay data plane, and tracked
graceful shutdown.

Implemented local protections include:

- strict, bounded UTF-8 JSON and one-request-per-connection IPC;
- a `0660` Unix socket in a trusted daemon-owned directory;
- safe stale-socket replacement, connection limits, deadlines, and exception
  containment;
- daemon-owned state and credential databases with exact `0600` permissions;
- refusal of symbolic links, multiply-linked files, non-regular files, ownership
  mismatches, and group/world-writable parent directories;
- descriptor/path inode verification across SQLite open to detect path replacement;
- bound credential values, secure SQLite deletion, and schema refusal rather than
  destructive rebuilding;
- non-echoing interactive Token input and explicit `--token-stdin` automation;
- no Token in normal output, JSON responses, state rows, errors, or logs;
- proactive cleansing of Token-bearing CLI, IPC, and `SecureString` buffers.

The credential database is permission-protected but not encrypted. Root, the daemon
account, a compromised process with equivalent access, backups, or storage snapshots
may still expose credentials. Deployments must protect the host and filesystem and
must not place credential files in shared directories.

Remote control transport requires TLS 1.2 or newer, hostname verification, HMAC-SHA256
challenge authentication, bounded nonce replay protection, clock-skew validation,
failure throttling, fresh session generations, and heartbeat deadlines. Each public
server has an isolated daemon session, reconnect controller, and Worker Pool.

Public listeners use numeric-only bind addresses, a required finite `--allow-ports`
policy, per-client registered-tunnel limits, generation-scoped listener ownership, and
stable `permission_denied`, `resource_exhausted`, and `remote_port_in_use` failures.
Local target hosts and ports never cross the control protocol.

Worker identity is validated against the current session generation, with
per-session and global idle limits, bounded public-socket waiting, automatic
replenishment, and idle expiration. Stale Workers are closed when their generation is
replaced.

An assigned Worker resolves only the locally persisted tunnel target. Remote
messages cannot select an arbitrary local host or port. Local connection failures are
generic, relay buffers are fixed at 16 KiB per direction, and inactivity deadlines
bound otherwise silent connections.

MiniTun enforces per-client and global public-relay quotas, a daemon-wide Worker/relay
connection budget, strand-serialized session ownership, bounded pending-connection
lifetimes, best-effort `GOAWAY`, and deadline-enforced relay draining. Capacity uses
move-only leases so rejection, timeout, cancellation, half-close, and ordinary teardown
cannot leak or double-release a quota slot.

The complete suite is validated under AddressSanitizer, UndefinedBehaviorSanitizer,
and ThreadSanitizer. TLS stream objects remain alive until all cancelled asynchronous
operations have completed, including during `GOAWAY` shutdown. Dedicated libFuzzer
targets exercise remote frames, IPC frames, IPC JSON, endpoints, and port ranges.
