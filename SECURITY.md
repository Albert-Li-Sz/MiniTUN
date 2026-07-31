# Security policy

MiniTun is pre-release software. Do not deploy the current stage-2 build as a tunnel
service. The network-facing runtime, TLS authentication, credential backend, IPC
permissions, and installation-time filesystem ownership and permissions have not yet
been implemented.

The stage-2 SQLite schema contains an optional `credential_ref`. It is an opaque
identifier for a future protected credential backend, not a storage location for a
token, password, private key, certificate, or other secret. The schema cannot determine
whether a caller supplied secret material, so callers must never persist credential
material in this field. The current stage does not provide a supported way to log in or
store a credential.

The storage layer uses bound parameters, validates records at repository boundaries,
enforces schema constraints and foreign keys, and performs migrations and restart-state
normalization in transactions. It refuses unsupported, malformed, or unversioned
non-empty databases instead of deleting or rebuilding them. These protections do not
replace the database-file permission checks and protected credential storage planned
for later security and installation stages.

Please report vulnerabilities privately through the GitHub repository's security
advisory feature. Do not include tokens, private keys, or other live credentials in an
issue, log, test fixture, or proof of concept.
