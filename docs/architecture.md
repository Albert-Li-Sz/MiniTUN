# Architecture

MiniTun will consist of three Linux programs:

- `minitun`: a stateless, short-lived CLI that communicates only with the local daemon.
- `minitund`: the client daemon that owns persistence, credentials, server sessions,
  tunnel state, and local relay connections.
- `minitun-server`: the public TLS endpoint and remote TCP listener manager.

Each configured public server will have an isolated `ServerSession`, including its own
control connection, authentication state, heartbeat, reconnection controller, worker
pool, session generation, and tunnel registry. A failure in one session must not affect
another session.

The stage-1 common layer provides the shared error/result model, structured logging,
validated endpoints and port ranges, typed random IDs, time helpers, and move-only
secret storage. Runtime components will be added and tested in the ordered phases
described by the project requirements.
