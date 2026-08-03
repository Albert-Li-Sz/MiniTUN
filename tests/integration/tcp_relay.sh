#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-relay.XXXXXX")
socket_path="$runtime_dir/minitun.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
daemon_pid=
server_pid=
echo_pid=

cleanup() {
    for process_id in "$daemon_pid" "$server_pid" "$echo_pid"; do
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
token='stage-ten-relay-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

read -r control_port local_port remote_port failed_remote_port < <(python3 - <<'PY'
import socket

ports = []
for _ in range(4):
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

def handle(connection):
    try:
        payload = bytearray()
        while True:
            chunk = connection.recv(16384)
            if not chunk:
                break
            payload.extend(chunk)
        if payload:
            connection.sendall(payload)
        connection.shutdown(socket.SHUT_WR)
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
    threading.Thread(target=handle, args=(connection,), daemon=True).start()
listener.close()
PY
echo_pid=$!

start_server() {
    "$server_bin" --foreground \
        --listen "127.0.0.1:$control_port" \
        --tls-cert "$runtime_dir/server.crt" \
        --tls-key "$runtime_dir/server.key" \
        --token-file "$runtime_dir/token" \
        --allow-ports 1024-65535 \
        --heartbeat-interval 1 \
        --heartbeat-timeout 3 \
        --relay-idle-timeout 2 \
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
        --relay-idle-timeout 2 \
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

stop_daemon() {
    kill -TERM "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
}

wait_tunnel() {
    local name=$1
    local expected=$2
    for _ in $(seq 1 160); do
        if "$minitun_bin" --socket "$socket_path" tun inspect "$name" --json 2>/dev/null |
            python3 -c "import json,sys; raise SystemExit(json.load(sys.stdin)['actual_state'] != '$expected')"
        then
            return
        fi
        sleep 0.1
    done
    sed -n '1,260p' "$runtime_dir/minitund.log" >&2
    return 1
}

round_trip() {
    local size=$1
    local concurrency=$2
    python3 - "$remote_port" "$size" "$concurrency" <<'PY'
import concurrent.futures
import hashlib
import socket
import sys
import time

port = int(sys.argv[1])
size = int(sys.argv[2])
concurrency = int(sys.argv[3])
started = time.monotonic()

def transfer(index):
    seed = bytes(((offset + index) % 251 for offset in range(251)))
    payload = (seed * ((size + len(seed) - 1) // len(seed)))[:size]
    last_error = None
    for _ in range(30):
        try:
            connection = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            connection.settimeout(8)
            connection.sendall(payload)
            connection.shutdown(socket.SHUT_WR)
            received = bytearray()
            while True:
                chunk = connection.recv(16384)
                if not chunk:
                    break
                received.extend(chunk)
            connection.close()
            if hashlib.sha256(received).digest() != hashlib.sha256(payload).digest():
                raise RuntimeError("relay payload hash mismatch")
            return
        except (OSError, RuntimeError) as error:
            last_error = error
            time.sleep(0.1)
    raise last_error

with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
    list(executor.map(transfer, range(concurrency)))

elapsed = time.monotonic() - started
transferred_mib = (size * concurrency * 2) / (1024 * 1024)
throughput = transferred_mib / max(elapsed, 0.001)
print(
    f"relay benchmark: {concurrency} connections, {elapsed:.3f}s, "
    f"{throughput:.1f} MiB/s aggregate duplex"
)
if elapsed > 15:
    raise SystemExit("concurrent relay latency exceeded the 15-second regression limit")
PY
}

start_server
start_daemon
"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" --name primary \
    >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary "$local_port" "$remote_port" \
    --name relay >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary 1 "$failed_remote_port" \
    --name unavailable >/dev/null
wait_tunnel relay active
wait_tunnel unavailable active

round_trip 2097152 1
round_trip 1048576 8

python3 - "$remote_port" <<'PY'
import socket
import sys
import time

connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.5)
connection.settimeout(5)
time.sleep(3)
if connection.recv(1) != b"":
    raise SystemExit(1)
connection.close()
PY

python3 - "$failed_remote_port" <<'PY'
import socket
import sys

connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.5)
connection.settimeout(1)
if connection.recv(1) != b"":
    raise SystemExit(1)
connection.close()
PY

stop_daemon
start_daemon
wait_tunnel relay active
round_trip 131072 2

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
start_server
wait_tunnel relay active
round_trip 131072 2

"$minitun_bin" --socket "$socket_path" tun remove relay >/dev/null
for _ in $(seq 1 80); do
    if python3 - "$remote_port" <<'PY'
import socket
import sys
probe = socket.socket()
probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
probe.bind(("0.0.0.0", int(sys.argv[1])))
probe.close()
PY
    then
        break
    fi
    sleep 0.1
done

if grep -F "$token" "$runtime_dir"/*.log; then
    printf 'authentication token leaked into relay logs\n' >&2
    exit 1
fi

stop_daemon
echo 'TCP relay integration passed'
