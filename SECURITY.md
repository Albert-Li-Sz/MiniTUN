# Security policy

MiniTun 0.1.0 has completed the stage-16 security and release-readiness checks recorded
in [docs/acceptance.md](docs/acceptance.md). Until a stable tag is published, security
fixes apply to the current `main` branch; older development snapshots are unsupported.
Acceptance demonstrates the documented controls and test coverage, but is not a
security guarantee.

MiniTun requires TLS 1.2 or newer, certificate and hostname verification, HMAC-SHA256
challenge authentication, bounded nonce replay protection, clock-skew checks, and
authentication throttling. Each configured server has an isolated control session,
generation-scoped Worker Pool, tunnel registry, reconnect controller, and resource
budget. Relay queues are bounded, inactivity and shutdown deadlines are enforced, and
public listeners require an explicit port allowlist.

`minitun` communicates only with `minitund` over a bounded Unix-socket protocol. The
daemon requires trusted socket and database paths, creates the socket as `0660`, and
stores state and credentials in separate daemon-owned SQLite files with exact `0600`
permissions. Tokens are excluded from state rows, IPC responses, errors, normal logs,
and command arguments; interactive input disables terminal echo and transient secret
buffers are proactively cleansed. See [docs/security.md](docs/security.md) for the
complete design and threat boundaries.

The credential database is permission-protected but not encrypted. These controls do
not protect against root, the daemon account, host compromise, privileged memory
inspection, or insecure backups. Operators must protect the host, provision a trusted
CA and private authentication material, restrict the public port allowlist, and review
the installed systemd policy before exposing a service.

Please report vulnerabilities privately through the GitHub repository's security
advisory feature. Never include live Tokens, private keys, or certificates in an issue,
log, test fixture, or proof of concept.
