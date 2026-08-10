#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-stability.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
socket_path="$runtime_dir/minitun.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
daemon_pid=
server_pid=
echo_pid=
hold_pid=

cleanup() {
    for process_id in "$hold_pid" "$daemon_pid" "$server_pid" "$echo_pid"; do
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
token='stage-eleven-stability-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

read -r control_port local_port remote_port < <(python3 - <<'PY'
import socket

ports = []
for _ in range(3):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    ports.append(probe.getsockname()[1])
    probe.close()
print(*ports)
PY
)

python3 - "$local_port" <<'PY' &
import signal
import socket
import sys
import threading

listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", int(sys.argv[1])))
listener.listen()
listener.settimeout(0.2)
stopping = threading.Event()

def stop(_signum, _frame):
    stopping.set()

def echo(connection):
    try:
        while True:
            payload = connection.recv(16384)
            if not payload:
                break
            connection.sendall(payload)
    except OSError:
        pass
    finally:
        connection.close()

signal.signal(signal.SIGTERM, stop)
while not stopping.is_set():
    try:
        connection, _ = listener.accept()
    except TimeoutError:
        continue
    threading.Thread(target=echo, args=(connection,), daemon=True).start()
listener.close()
PY
echo_pid=$!

start_server() {
    "$server_bin" --foreground \
        --listen "127.0.0.1:$control_port" \
        --tls-cert "$runtime_dir/server.crt" \
        --tls-key "$runtime_dir/server.key" \
        --clients-config "$runtime_dir/clients.json" \
        --heartbeat-interval 1 \
        --heartbeat-timeout 3 \
        --worker-wait-timeout 1 \
        --min-idle-workers 2 \
        --max-idle-workers 4 \
        --max-total-idle-workers 8 \
        --max-connections-per-client 1 \
        --max-total-connections 1 \
        --shutdown-timeout 4 \
        --io-threads 4 \
        >>"$runtime_dir/server.log" 2>&1 &
    server_pid=$!
    for _ in $(seq 1 100); do
        kill -0 "$server_pid" 2>/dev/null || return 1
        python3 - "$control_port" <<'PY' 2>/dev/null && return
import socket
import sys
probe = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.1)
probe.close()
PY
        sleep 0.05
    done
    return 1
}

start_daemon() {
    "$minitund_bin" --foreground \
        --socket "$socket_path" \
        --database "$state_path" \
        --credentials "$credentials_path" \
        --tls-ca "$runtime_dir/server.crt" \
        --shutdown-timeout 4 \
        --io-threads 4 \
        >>"$runtime_dir/minitund.log" 2>&1 &
    daemon_pid=$!
    for _ in $(seq 1 100); do
        [[ -S "$socket_path" ]] && return
        kill -0 "$daemon_pid" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}

wait_tunnel() {
    for _ in $(seq 1 200); do
        if "$minitun_bin" --socket "$socket_path" tun inspect stable --json 2>/dev/null |
            python3 -c "import json,sys; raise SystemExit(json.load(sys.stdin)['actual_state'] != 'active')"
        then
            return
        fi
        sleep 0.1
    done
    sed -n '1,260p' "$runtime_dir/minitund.log" >&2
    return 1
}

round_trip() {
    python3 - "$remote_port" <<'PY'
import socket
import sys
import time

last_error = None
for _ in range(40):
    try:
        connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.5)
        connection.settimeout(2)
        connection.sendall(b"stable-round-trip")
        if connection.recv(64) != b"stable-round-trip":
            raise RuntimeError("relay payload mismatch")
        connection.close()
        raise SystemExit(0)
    except (OSError, RuntimeError) as error:
        last_error = error
        time.sleep(0.1)
raise last_error
PY
}

start_hold() {
    local ready_path=$1
    local release_path=$2
    python3 - "$remote_port" "$ready_path" "$release_path" <<'PY' &
import os
import socket
import sys
import time

connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1)
connection.settimeout(3)
connection.sendall(b"hold-open")
if connection.recv(64) != b"hold-open":
    raise SystemExit("initial held relay payload mismatch")
open(sys.argv[2], "wb").close()
while not os.path.exists(sys.argv[3]):
    time.sleep(0.02)
connection.sendall(b"still-open")
if connection.recv(64) != b"still-open":
    raise SystemExit("draining relay payload mismatch")
connection.close()
PY
    hold_pid=$!
    for _ in $(seq 1 100); do
        [[ -e "$ready_path" ]] && return
        kill -0 "$hold_pid" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}

wait_for_exit() {
    local process_id=$1
    for _ in $(seq 1 120); do
        if ! kill -0 "$process_id" 2>/dev/null; then
            wait "$process_id"
            return
        fi
        sleep 0.05
    done
    return 1
}

start_daemon
bash "$integration_dir/write_client_policy.sh" "$minitun_bin" "$socket_path" \
    "$runtime_dir/clients.json" "$runtime_dir/token" 1024-65535 128 1 4 >/dev/null
start_server
"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" --name primary \
    >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary "$local_port" "$remote_port" \
    --name stable >/dev/null
wait_tunnel

for _ in $(seq 1 50); do
    round_trip
done

start_hold "$runtime_dir/quota.ready" "$runtime_dir/quota.release"
python3 - "$remote_port" <<'PY'
import socket
import sys

connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1)
connection.settimeout(2)
try:
    connection.sendall(b"must-be-rejected")
    if connection.recv(1) != b"":
        raise SystemExit("connection quota admitted a second relay")
except (ConnectionResetError, BrokenPipeError):
    pass
finally:
    connection.close()
PY
touch "$runtime_dir/quota.release"
wait "$hold_pid"
hold_pid=
round_trip

start_hold "$runtime_dir/server-drain.ready" "$runtime_dir/server-drain.release"
kill -TERM "$server_pid"
sleep 0.2
kill -0 "$server_pid"
touch "$runtime_dir/server-drain.release"
wait "$hold_pid"
hold_pid=
wait_for_exit "$server_pid"
server_pid=

start_server
wait_tunnel
round_trip

start_hold "$runtime_dir/daemon-drain.ready" "$runtime_dir/daemon-drain.release"
kill -TERM "$daemon_pid"
sleep 0.2
kill -0 "$daemon_pid"
touch "$runtime_dir/daemon-drain.release"
wait "$hold_pid"
hold_pid=
wait_for_exit "$daemon_pid"
daemon_pid=

if grep -F "$token" "$runtime_dir"/*.log; then
    printf 'authentication token leaked into stability logs\n' >&2
    exit 1
fi

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
echo 'stability integration passed'
