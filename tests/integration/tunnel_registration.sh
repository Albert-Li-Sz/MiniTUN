#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-registration.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
socket_path="$runtime_dir/minitun.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
daemon_pid=
server_pid=
conflict_pid=
control_port=

cleanup() {
    for process_id in "$daemon_pid" "$server_pid" "$conflict_pid"; do
        if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
            kill -TERM "$process_id" 2>/dev/null || true
            wait "$process_id" 2>/dev/null || true
        fi
    done
    if [[ ${MINITUN_KEEP_TEST_ARTIFACTS:-0} == 1 ]]; then
        printf 'kept tunnel registration artifacts at %s\n' "$runtime_dir" >&2
    else
        rm -rf "$runtime_dir"
    fi
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost \
    -keyout "$runtime_dir/server.key" \
    -out "$runtime_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/server.key"
token='stage-eight-registration-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

python3 - "$runtime_dir/conflict-port" <<'PY' &
import random
import signal
import socket
import sys
import time

listener = None
candidates = list(range(10000, 24000))
random.SystemRandom().shuffle(candidates)
for candidate in candidates:
    probe = socket.socket()
    try:
        probe.bind(("0.0.0.0", candidate))
    except OSError:
        probe.close()
        continue
    listener = probe
    break
if listener is None:
    raise SystemExit("unable to reserve a non-ephemeral conflict port")
listener.listen()
with open(sys.argv[1], "w", encoding="ascii") as stream:
    stream.write(str(listener.getsockname()[1]))

running = True
def stop(_signum, _frame):
    global running
    running = False

signal.signal(signal.SIGTERM, stop)
while running:
    time.sleep(0.1)
listener.close()
PY
conflict_pid=$!
for _ in $(seq 1 100); do
    [[ -s "$runtime_dir/conflict-port" ]] && break
    sleep 0.02
done
conflict_port=$(<"$runtime_dir/conflict-port")
active_port=$(python3 - "$conflict_port" <<'PY'
import random
import socket
import sys

# Public tunnel listeners must stay outside Linux's usual ephemeral source-port
# range. Readiness connections could otherwise claim a just-released listener
# port before the asynchronous unregister check observes that it is reusable.
conflict_port = int(sys.argv[1])
candidates = list(range(10000, 24000))
random.SystemRandom().shuffle(candidates)
for candidate in candidates:
    if candidate == conflict_port:
        continue
    probe = socket.socket()
    try:
        probe.bind(("0.0.0.0", candidate))
    except OSError:
        probe.close()
        continue
    print(candidate)
    probe.close()
    break
else:
    raise SystemExit("unable to select a non-ephemeral tunnel port")
PY
)

start_server() {
    local requested_port=${1:-}
    local candidate
    if [[ -n "$requested_port" ]]; then
        candidates=$requested_port
    else
        base_port=$((30000 + ($$ % 5000)))
        candidates=$(seq "$base_port" $((base_port + 40)))
    fi
    for candidate in $candidates; do
        "$server_bin" --foreground \
            --listen "127.0.0.1:$candidate" \
            --tls-cert "$runtime_dir/server.crt" \
            --tls-key "$runtime_dir/server.key" \
            --clients-config "$runtime_dir/clients.json" \
            --heartbeat-interval 60 \
            --heartbeat-timeout 180 \
            --io-threads 2 \
            >>"$runtime_dir/server.log" 2>&1 &
        server_pid=$!
        sleep 0.1
        if kill -0 "$server_pid" 2>/dev/null; then
            control_port=$candidate
            return
        fi
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    done
    sed -n '1,200p' "$runtime_dir/server.log" >&2
    exit 1
}

start_daemon() {
    "$minitund_bin" --foreground \
        --socket "$socket_path" \
        --database "$state_path" \
        --credentials "$credentials_path" \
        --tls-ca "$runtime_dir/server.crt" \
        --io-threads 4 \
        >>"$runtime_dir/minitund.log" 2>&1 &
    daemon_pid=$!
    for _ in $(seq 1 100); do
        [[ -S "$socket_path" ]] && return
        if ! kill -0 "$daemon_pid" 2>/dev/null; then
            sed -n '1,240p' "$runtime_dir/minitund.log" >&2
            exit 1
        fi
        sleep 0.05
    done
    exit 1
}

stop_daemon() {
    kill -TERM "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
}

wait_tunnel_state() {
    local name=$1
    local state=$2
    local error_code=${3:-}
    local output="$runtime_dir/tunnel-$name.json"
    for _ in $(seq 1 120); do
        if "$minitun_bin" --socket "$socket_path" tun inspect "$name" --json \
                >"$output" 2>/dev/null &&
            python3 - "$output" "$state" "$error_code" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    tunnel = json.load(stream)
if tunnel["actual_state"] != sys.argv[2]:
    raise SystemExit(1)
expected_error = sys.argv[3]
if expected_error and (not tunnel["last_error"] or
                       tunnel["last_error"]["code"] != expected_error):
    raise SystemExit(1)
if tunnel["actual_state"] in {"active", "failed"}:
    if tunnel["server_actual_state"] != "online":
        raise SystemExit(1)
    if tunnel["pending_reason"] is not None:
        raise SystemExit(1)
    if not isinstance(tunnel["last_synced_at"], int):
        raise SystemExit(1)
if tunnel["actual_state"] == "pending":
    if not isinstance(tunnel["server_actual_state"], str):
        raise SystemExit(1)
    if not isinstance(tunnel["pending_reason"], str):
        raise SystemExit(1)
    if not isinstance(tunnel["last_synced_at"], int):
        raise SystemExit(1)
PY
        then
            return
        fi
        sleep 0.1
    done
    cat "$output" >&2 || true
    sed -n '1,240p' "$runtime_dir/minitund.log" >&2
    exit 1
}

wait_port_open() {
    local port=$1
    for _ in $(seq 1 80); do
        if python3 - "$port" <<'PY'
import socket
import sys

connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2)
connection.close()
PY
        then
            return
        fi
        sleep 0.1
    done
    exit 1
}

wait_port_released() {
    local port=$1
    for _ in $(seq 1 80); do
        if python3 - "$port" <<'PY'
import socket
import sys

probe = socket.socket()
probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
probe.bind(("0.0.0.0", int(sys.argv[1])))
probe.close()
PY
        then
            return
        fi
        sleep 0.1
    done
    exit 1
}

start_daemon
bash "$integration_dir/write_client_policy.sh" "$minitun_bin" "$socket_path" \
    "$runtime_dir/clients.json" "$runtime_dir/token" >/dev/null
start_server
"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" --name primary \
    >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin >/dev/null

"$minitun_bin" --socket "$socket_path" tun add primary 22 "$active_port" --name active \
    >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary 23 80 --name denied >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary 24 "$conflict_port" --name conflict \
    >/dev/null
wait_tunnel_state active active
wait_tunnel_state denied failed permission_denied
wait_tunnel_state conflict failed remote_port_in_use
wait_port_open "$active_port"

kill -TERM "$conflict_pid"
wait "$conflict_pid"
conflict_pid=
# With a 60-second heartbeat, this committed removal must wake the control
# session immediately. The same reconciliation pass retries the failed port.
"$minitun_bin" --socket "$socket_path" tun remove denied >/dev/null
wait_tunnel_state conflict active
wait_port_open "$conflict_port"

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
wait_tunnel_state active pending
start_server "$control_port"
wait_tunnel_state active active
wait_tunnel_state conflict active

stop_daemon
start_daemon
wait_tunnel_state active active
wait_tunnel_state conflict active

python3 - "$state_path" <<'PY'
import sqlite3
import sys

database = sqlite3.connect(sys.argv[1])
database.execute(
    "CREATE TRIGGER reject_pending_persistence "
    "BEFORE UPDATE OF actual_state ON tunnels "
    "WHEN NEW.actual_state = 'pending' BEGIN "
    "SELECT RAISE(ABORT, 'injected pending persistence failure'); END"
)
database.commit()
database.close()
PY

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
persistence_error_logged=false
for _ in $(seq 1 100); do
    if grep -F 'failed to persist pending tunnel states after disconnect' \
            "$runtime_dir/minitund.log" >/dev/null; then
        persistence_error_logged=true
        break
    fi
    sleep 0.05
done
if [[ "$persistence_error_logged" != true ]]; then
    sed -n '1,280p' "$runtime_dir/minitund.log" >&2
    exit 1
fi

python3 - "$state_path" <<'PY'
import sqlite3
import sys

database = sqlite3.connect(sys.argv[1])
database.execute("DROP TRIGGER reject_pending_persistence")
database.commit()
database.close()
PY

"$minitun_bin" --socket "$socket_path" tun remove active >/dev/null
"$minitun_bin" --socket "$socket_path" tun remove conflict >/dev/null
wait_port_released "$active_port"
wait_port_released "$conflict_port"

if grep -F "$token" "$runtime_dir"/*.log; then
    printf 'authentication token leaked into registration logs\n' >&2
    exit 1
fi

stop_daemon
echo 'tunnel registration integration passed'
