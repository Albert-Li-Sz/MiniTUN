#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-multi-server.XXXXXX")
socket_path="$runtime_dir/minitun.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
daemon_pid=
server_a_pid=
server_b_pid=
port_a=
port_b=

cleanup() {
    for process_id in "$daemon_pid" "$server_a_pid" "$server_b_pid"; do
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

token='stage-seven-multi-server-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

start_server() {
    local label=$1
    local requested_port=$2
    local output_var=$3
    local process_var=$4
    local candidate
    local process_id

    for offset in $(seq 0 40); do
        candidate=$((requested_port + offset))
        "$server_bin" --foreground \
            --listen "127.0.0.1:$candidate" \
            --tls-cert "$runtime_dir/server.crt" \
            --tls-key "$runtime_dir/server.key" \
            --token-file "$runtime_dir/token" \
            --heartbeat-interval 1 \
            --heartbeat-timeout 3 \
            --io-threads 2 \
            >>"$runtime_dir/server-$label.log" 2>&1 &
        process_id=$!
        sleep 0.1
        if kill -0 "$process_id" 2>/dev/null; then
            printf -v "$output_var" '%s' "$candidate"
            printf -v "$process_var" '%s' "$process_id"
            return
        fi
        wait "$process_id" 2>/dev/null || true
    done
    printf 'failed to start server %s\n' "$label" >&2
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
        if [[ -S "$socket_path" ]]; then
            return
        fi
        if ! kill -0 "$daemon_pid" 2>/dev/null; then
            sed -n '1,200p' "$runtime_dir/minitund.log" >&2
            exit 1
        fi
        sleep 0.05
    done
    printf 'timed out waiting for minitund\n' >&2
    exit 1
}

stop_daemon() {
    kill -TERM "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
}

wait_for_states() {
    local expected_a=$1
    local expected_b=$2
    local listing="$runtime_dir/server-list.json"
    for _ in $(seq 1 120); do
        if "$minitun_bin" --socket "$socket_path" server list --json >"$listing" 2>/dev/null &&
            python3 - "$listing" "$expected_a" "$expected_b" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    servers = {item["name"]: item for item in json.load(stream)}
if set(servers) != {"primary", "backup"}:
    raise SystemExit(1)
if sys.argv[2] == "offline":
    if servers["primary"]["actual_state"] == "online":
        raise SystemExit(1)
elif servers["primary"]["actual_state"] != sys.argv[2]:
    raise SystemExit(1)
if servers["backup"]["actual_state"] != sys.argv[3]:
    raise SystemExit(1)
PY
        then
            return
        fi
        sleep 0.1
    done
    cat "$listing" >&2 || true
    sed -n '1,240p' "$runtime_dir/minitund.log" >&2
    printf 'server states did not become %s/%s\n' "$expected_a" "$expected_b" >&2
    exit 1
}

base_port=$((26000 + ($$ % 5000)))
start_server a "$base_port" port_a server_a_pid
start_server b $((base_port + 100)) port_b server_b_pid
start_daemon

"$minitun_bin" --socket "$socket_path" server add "localhost:$port_a" --name primary >/dev/null
"$minitun_bin" --socket "$socket_path" server add "localhost:$port_b" --name backup >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login backup --token-stdin >/dev/null
wait_for_states online online

client_id_before=$(python3 - "$state_path" <<'PY'
import sqlite3
import sys

connection = sqlite3.connect(sys.argv[1])
try:
    print(connection.execute("SELECT client_id FROM daemon_identity").fetchone()[0])
finally:
    connection.close()
PY
)

kill -TERM "$server_a_pid"
wait "$server_a_pid"
server_a_pid=
wait_for_states offline online

start_server a "$port_a" port_a server_a_pid
wait_for_states online online

stop_daemon
start_daemon
wait_for_states online online

client_id_after=$(python3 - "$state_path" <<'PY'
import sqlite3
import sys

connection = sqlite3.connect(sys.argv[1])
try:
    print(connection.execute("SELECT client_id FROM daemon_identity").fetchone()[0])
finally:
    connection.close()
PY
)
if [[ "$client_id_before" != "$client_id_after" ]]; then
    printf 'daemon client identity changed across restart\n' >&2
    exit 1
fi

if grep -F "$token" "$runtime_dir"/*.log; then
    printf 'authentication token leaked into logs\n' >&2
    exit 1
fi

stop_daemon
echo 'multi-server session integration passed'
