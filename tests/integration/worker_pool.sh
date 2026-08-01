#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-workers.XXXXXX")
socket_path="$runtime_dir/minitun.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
daemon_pid=
server_pid=

cleanup() {
    for process_id in "$daemon_pid" "$server_pid"; do
        if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
            kill -TERM "$process_id" 2>/dev/null || true
            wait "$process_id" 2>/dev/null || true
        fi
    done
    rm -rf "$runtime_dir"
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost \
    -keyout "$runtime_dir/server.key" \
    -out "$runtime_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/server.key"
token='stage-nine-worker-pool-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

read -r control_port remote_port < <(python3 - <<'PY'
import socket

ports = []
for _ in range(2):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    ports.append(probe.getsockname()[1])
    probe.close()
print(*ports)
PY
)

"$server_bin" --foreground \
    --listen "127.0.0.1:$control_port" \
    --tls-cert "$runtime_dir/server.crt" \
    --tls-key "$runtime_dir/server.key" \
    --token-file "$runtime_dir/token" \
    --allow-ports 1024-65535 \
    --heartbeat-interval 1 \
    --heartbeat-timeout 3 \
    --min-idle-workers 1 \
    --max-idle-workers 1 \
    --max-total-idle-workers 1 \
    --worker-idle-timeout 2 \
    --io-threads 2 \
    >"$runtime_dir/server.log" 2>&1 &
server_pid=$!

"$minitund_bin" --foreground \
    --socket "$socket_path" \
    --database "$state_path" \
    --credentials "$credentials_path" \
    --tls-ca "$runtime_dir/server.crt" \
    --io-threads 4 \
    >"$runtime_dir/minitund.log" 2>&1 &
daemon_pid=$!
tunnel_ready=false
for _ in $(seq 1 100); do
    [[ -S "$socket_path" ]] && break
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
        sed -n '1,240p' "$runtime_dir/minitund.log" >&2
        exit 1
    fi
    sleep 0.05
done

"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" --name primary \
    >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary 9 "$remote_port" --name workers \
    >/dev/null

for _ in $(seq 1 100); do
    if "$minitun_bin" --socket "$socket_path" tun inspect workers --json 2>/dev/null |
        python3 -c 'import json,sys; raise SystemExit(json.load(sys.stdin)["actual_state"] != "active")'
    then
        tunnel_ready=true
        break
    fi
    sleep 0.1
done
[[ "$tunnel_ready" == true ]]

probe_worker() {
    python3 - "$remote_port" <<'PY'
import socket
import sys
import time

started = time.monotonic()
connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.5)
connection.settimeout(0.75)
if connection.recv(1) != b"":
    raise SystemExit(1)
if time.monotonic() - started >= 0.75:
    raise SystemExit(1)
connection.close()
PY
}

worker_ready=false
for _ in $(seq 1 20); do
    if probe_worker; then
        worker_ready=true
        break
    fi
    sleep 0.1
done
[[ "$worker_ready" == true ]]
sleep 1.3
probe_worker
sleep 3.5
probe_worker

if grep -F "$token" "$runtime_dir"/*.log; then
    printf 'authentication token leaked into Worker Pool logs\n' >&2
    exit 1
fi

kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=
echo 'Worker Pool integration passed'
