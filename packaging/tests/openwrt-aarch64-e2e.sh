#!/usr/bin/env bash
set -euo pipefail

qemu_binary=${1:?missing QEMU executable}
runtime_root=${2:?missing OpenWrt runtime root}
minitun_bin=${3:?missing OpenWrt minitun binary}
minitund_bin=${4:?missing OpenWrt minitund binary}
server_bin=${5:?missing OpenWrt minitun-server binary}

for command_name in openssl python3; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf '%s is required for the OpenWrt AArch64 E2E test\n' "$command_name" >&2
        exit 2
    }
done

runtime_base=$(cd "${TMPDIR:-/tmp}" && pwd -P)
test_dir=$(mktemp -d "$runtime_base/minitun-openwrt-aarch64.XXXXXX")
socket_path="$test_dir/minitun.sock"
state_path="$test_dir/state.db"
credentials_path="$test_dir/credentials.db"
daemon_pid=
server_pid=
echo_pid=
qemu=("$qemu_binary" -L "$runtime_root")

cleanup() {
    status=$?
    for process_id in "$daemon_pid" "$server_pid" "$echo_pid"; do
        if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
            kill -TERM "$process_id" 2>/dev/null || true
            wait "$process_id" 2>/dev/null || true
        fi
    done
    if [[ "$status" -ne 0 ]]; then
        printf '%s\n' 'OpenWrt AArch64 E2E logs:' >&2
        for log_file in "$test_dir"/*.log; do
            [[ -f "$log_file" ]] || continue
            printf '%s\n' "--- $log_file" >&2
            sed -n '1,240p' "$log_file" >&2
        done
    fi
    rm -rf "$test_dir"
    return "$status"
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost \
    -keyout "$test_dir/server.key" \
    -out "$test_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$test_dir/server.key"
token='openwrt-aarch64-runtime-token'
printf '%s\n' "$token" >"$test_dir/token"
chmod 0600 "$test_dir/token"

read -r control_port local_port remote_port < <(python3 - <<'PY'
import random
import socket

control_probe = socket.socket()
local_probe = socket.socket()
control_probe.bind(("127.0.0.1", 0))
local_probe.bind(("127.0.0.1", 0))
control = control_probe.getsockname()[1]
local = local_probe.getsockname()[1]
try:
    candidates = list(range(10000, 24000))
    random.SystemRandom().shuffle(candidates)
    for candidate in candidates:
        if candidate in {control, local}:
            continue
        probe = socket.socket()
        try:
            probe.bind(("0.0.0.0", candidate))
        except OSError:
            probe.close()
            continue
        probe.close()
        print(control, local, candidate)
        break
    else:
        raise SystemExit("unable to select a public tunnel port")
finally:
    control_probe.close()
    local_probe.close()
PY
)

python3 - "$local_port" <<'PY' >"$test_dir/echo.log" 2>&1 &
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
    threading.Thread(target=echo, args=(connection,), daemon=True).start()
listener.close()
PY
echo_pid=$!

"${qemu[@]}" "$server_bin" --foreground \
    --listen "127.0.0.1:$control_port" \
    --tls-cert "$test_dir/server.crt" \
    --tls-key "$test_dir/server.key" \
    --token-file "$test_dir/token" \
    --heartbeat-interval 1 \
    --heartbeat-timeout 3 \
    --worker-idle-timeout 70 \
    --min-idle-workers 1 \
    --max-idle-workers 2 \
    --io-threads 2 >"$test_dir/server.log" 2>&1 &
server_pid=$!

server_ready=false
for _ in $(seq 1 120); do
    kill -0 "$server_pid" 2>/dev/null || exit 1
    if python3 - "$control_port" <<'PY' 2>/dev/null
import socket
import sys
connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.1)
connection.close()
PY
    then
        server_ready=true
        break
    fi
    sleep 0.05
done
[[ "$server_ready" == true ]]

start_daemon() {
    if [[ -e "$socket_path" || -L "$socket_path" ]]; then
        unlink "$socket_path"
    fi
    "${qemu[@]}" "$minitund_bin" --foreground \
        --socket "$socket_path" \
        --database "$state_path" \
        --credentials "$credentials_path" \
        --tls-ca "$test_dir/server.crt" \
        --io-threads 2 >>"$test_dir/minitund.log" 2>&1 &
    daemon_pid=$!
    for _ in $(seq 1 160); do
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

run_cli() {
    "${qemu[@]}" "$minitun_bin" --socket "$socket_path" "$@"
}

wait_tunnel_active() {
    for _ in $(seq 1 200); do
        if run_cli tun inspect relay --json 2>/dev/null | python3 -c '
import json
import sys
tunnel = json.load(sys.stdin)
valid = (
    tunnel["actual_state"] == "active"
    and tunnel["server_actual_state"] == "online"
    and tunnel["pending_reason"] is None
    and isinstance(tunnel["last_synced_at"], int)
)
raise SystemExit(0 if valid else 1)
'; then
            return
        fi
        sleep 0.1
    done
    return 1
}

round_trip() {
    python3 - "$remote_port" <<'PY'
import socket
import sys
import time

payload = b"MiniTun OpenWrt AArch64 TLS/SQLite/TCP E2E"
last_error = None
for _ in range(50):
    try:
        connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.4)
        connection.settimeout(3)
        connection.sendall(payload)
        connection.shutdown(socket.SHUT_WR)
        received = bytearray()
        while True:
            chunk = connection.recv(16384)
            if not chunk:
                break
            received.extend(chunk)
        connection.close()
        if bytes(received) != payload:
            raise RuntimeError("tunnel payload mismatch")
        break
    except (OSError, RuntimeError) as error:
        last_error = error
        time.sleep(0.1)
else:
    raise last_error
PY
}

start_daemon
run_cli server add "localhost:$control_port" --name primary >/dev/null
printf '%s\n' "$token" | run_cli server login primary --token-stdin >/dev/null
run_cli tun add primary "$local_port" "$remote_port" --name relay >/dev/null
wait_tunnel_active
round_trip

python3 - "$state_path" "$credentials_path" <<'PY'
import sqlite3
import sys

state = sqlite3.connect(sys.argv[1])
credentials = sqlite3.connect(sys.argv[2])
if state.execute("SELECT MAX(version) FROM schema_version").fetchone()[0] != 3:
    raise SystemExit("unexpected state schema version")
row = state.execute(
    "SELECT actual_state, last_synced_at FROM tunnels WHERE desired_state = 'active'"
).fetchone()
if row is None or row[0] != "active" or not isinstance(row[1], int):
    raise SystemExit("active tunnel state was not persisted")
if credentials.execute("SELECT COUNT(*) FROM credentials").fetchone()[0] != 1:
    raise SystemExit("server credential was not persisted")
state.close()
credentials.close()
PY

stop_daemon

# Recreate the state produced by the previous release, then require the target
# SQLite library to migrate the populated database in place on restart.
python3 - "$state_path" <<'PY'
import sqlite3
import sys

state = sqlite3.connect(sys.argv[1])
state.executescript(
    "ALTER TABLE tunnels DROP COLUMN last_synced_at;"
    "DELETE FROM schema_version WHERE version = 3;"
)
state.commit()
state.close()
PY

start_daemon
wait_tunnel_active
round_trip

if grep -F "$token" "$test_dir"/*.log; then
    printf '%s\n' 'authentication token leaked into OpenWrt runtime logs' >&2
    exit 1
fi

stop_daemon
printf '%s\n' 'OpenWrt AArch64 runtime E2E passed'
