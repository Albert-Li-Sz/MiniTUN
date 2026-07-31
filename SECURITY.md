# Security policy

MiniTun is pre-release software. Do not deploy the stage-5 build as a public tunnel
service: TLS authentication, session isolation, port policy, relay limits, service
accounts, and installation hardening are not complete.

Stage 5 provides a functional local security boundary and a bounded remote protocol
parser. `minitun` talks only to a
strict, bounded Unix-socket protocol. `minitund` requires a trusted daemon-owned socket
directory, creates the socket as `0660`, bounds clients and request time, contains
malformed input and handler exceptions, and safely handles stale socket paths.

Only `minitund` opens SQLite. Server and tunnel changes are validated and transactional.
Authentication Tokens are stored in a separate daemon-owned regular file with mode
`0600`, never in `state.db`. The CLI disables terminal echo by default and requires an
explicit `--token-stdin` for pipelines. Token values are omitted from responses,
errors, and logs, and transient Token-bearing buffers are proactively cleansed.

The credential database is not encrypted. Filesystem permissions and memory cleansing
reduce accidental exposure but do not protect against root, the daemon account, host
compromise, privileged memory inspection, or insecure backups. Use only test Tokens
until the remote authentication and packaging stages are complete.

Please report vulnerabilities privately through the GitHub repository's security
advisory feature. Never include live Tokens, private keys, or certificates in an issue,
log, test fixture, or proof of concept.
