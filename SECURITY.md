# Security policy

MiniTun is pre-release software. Do not deploy the current stage-3 build as a tunnel
service. The network-facing runtime, TLS authentication, credential backend, service
account creation, and installation-time filesystem ownership have not yet been
implemented.

Stage 3 adds a local Unix-domain socket with a strict `0660` mode, a 1 MiB message
limit, bounded connection count, strict UTF-8 JSON schemas, one request per connection,
request deadlines, and exception containment. Startup refuses symbolic links in the
directory chain and symlink or non-socket entries at the configured path; shutdown
removes the path only while it still identifies the socket created by that server
instance. Malformed clients are disconnected without terminating the daemon or other
sessions.

The intended production path is `/run/minitun/minitun.sock`, owned by
`minitun:minitun`. Until packaging creates the protected runtime directory and service
account, development runs must use a private directory and an explicit `--socket`
path. The daemon must run as the intended socket owner. The parent must be a real
directory owned by the daemon and must not be writable by group or other users;
ancestors must be owned by root or the daemon, and writable ancestors require sticky
directory protection. A daemon-owned `0600` sidecar lock serializes socket replacement
and remains locked for the server lifetime. Socket mode is not a substitute for
protecting its directory namespace.

The stage-2 SQLite schema contains an optional `credential_ref`. It is an opaque
identifier for a future protected credential backend, not a storage location for a
token, password, private key, certificate, or other secret. The schema cannot determine
whether a caller supplied secret material, so callers must never persist credential
material in this field. The current stage does not provide a supported way to log in or
store a credential. The IPC implementation does not log request bodies, and future
credential methods must never return tokens in an IPC response.

The storage layer uses bound parameters, validates records at repository boundaries,
enforces schema constraints and foreign keys, and performs migrations and restart-state
normalization in transactions. It refuses unsupported, malformed, or unversioned
non-empty databases instead of deleting or rebuilding them. These protections do not
replace the database-file permission checks and protected credential storage planned
for later security and installation stages.

Please report vulnerabilities privately through the GitHub repository's security
advisory feature. Do not include tokens, private keys, or other live credentials in an
issue, log, test fixture, or proof of concept.
