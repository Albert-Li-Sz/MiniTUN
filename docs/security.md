# Security design status

Stage 1 establishes structured logging, a unified error model, OpenSSL-backed random
identifiers, bounded endpoint parsing, overflow-safe timestamp checks, and move-only
secret buffers that are cleansed before release. It does not yet provide a deployable
security boundary.

Planned security work includes TLS certificate verification, HMAC-based authentication,
nonce replay prevention, timestamp validation, authentication throttling, protected
credential storage, Unix-socket authorization, database/file permission checks, port
allow-lists, bounded frames, and resource quotas. Secrets must never be written to logs
or returned by IPC.
