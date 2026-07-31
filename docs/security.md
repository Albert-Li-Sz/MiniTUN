# Security design status

Stage 4 provides a protected local control plane, not end-to-end tunnel security.

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

The remote transport is not implemented yet. There is no TLS connection, remote
authentication, replay prevention, heartbeat, port authorization, or public TCP relay
in the stage-4 build. Those controls begin with the binary protocol and TLS stages.
