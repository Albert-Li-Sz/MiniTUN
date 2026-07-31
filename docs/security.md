# Security design status

Stage 7 provides a protected local control plane and authenticated TLS control
sessions, but not yet the TCP relay data plane.

Implemented local protections include:

- strict, bounded UTF-8 JSON and one-request-per-connection IPC;
- a `0660` Unix socket in a trusted daemon-owned directory;
- safe stale-socket replacement, connection limits, deadlines, and exception
  containment;
- daemon-only SQLite ownership and transactional resource mutations;
- a separate daemon-owned `0600` credential database;
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
server has an isolated daemon session and reconnect controller. Port authorization,
worker connections, and public TCP relay are not implemented until later stages.
