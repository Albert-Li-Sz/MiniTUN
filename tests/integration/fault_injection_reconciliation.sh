#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}
failpoint=${4:?missing failpoint name}

case "$failpoint" in
    daemon.after_registering_state_commit|daemon.after_register_request_write|daemon.after_register_response_receive)
        victim=daemon
        ;;
    server.after_listener_established)
        victim=server
        ;;
    *)
        printf 'unknown reconciliation failpoint: %s\n' "$failpoint" >&2
        exit 2
        ;;
esac

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-fault.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
socket_path="$runtime_dir/minitun.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
daemon_pid=
server_pid=
control_port=

cleanup() {
    for process_id in "$daemon_pid" "$server_pid"; do
        if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
            kill -TERM "$process_id" 2>/dev/null || true
            wait "$process_id" 2>/dev/null || true
        fi
    done
    if [[ ${MINITUN_KEEP_TEST_ARTIFACTS:-0} == 1 ]]; then
        printf 'kept fault-injection artifacts at %s\n' "$runtime_dir" >&2
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
token='fault-injection-reconciliation-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

remote_port=$(python3 <<'PY'
import random
import socket

candidates = list(range(12000, 24000))
random.SystemRandom().shuffle(candidates)
for candidate in candidates:
    probe = socket.socket()
    try:
        probe.bind(("127.0.0.1", candidate))
    except OSError:
        probe.close()
        continue
    print(candidate)
    probe.close()
    break
else:
    raise SystemExit("unable to select a public tunnel port")
PY
)

start_daemon() {
    local selected_failpoint=${1:-}
    rm -f "$socket_path"
    if [[ -n "$selected_failpoint" ]]; then
        MINITUN_TEST_FAILPOINT="$selected_failpoint" "$minitund_bin" --foreground \
            --socket "$socket_path" \
            --database "$state_path" \
            --credentials "$credentials_path" \
            --tls-ca "$runtime_dir/server.crt" \
            --io-threads 4 >>"$runtime_dir/minitund.log" 2>&1 &
    else
        "$minitund_bin" --foreground \
            --socket "$socket_path" \
            --database "$state_path" \
            --credentials "$credentials_path" \
            --tls-ca "$runtime_dir/server.crt" \
            --io-threads 4 >>"$runtime_dir/minitund.log" 2>&1 &
    fi
    daemon_pid=$!
    for _ in $(seq 1 160); do
        [[ -S "$socket_path" ]] && return
        if ! kill -0 "$daemon_pid" 2>/dev/null; then
            sed -n '1,240p' "$runtime_dir/minitund.log" >&2
            exit 1
        fi
        sleep 0.05
    done
    printf 'daemon did not become ready\n' >&2
    exit 1
}

start_server() {
    local selected_failpoint=${1:-}
    local candidate
    local base_port=$((30000 + ($$ % 8000)))
    local candidates
    if [[ -n "$control_port" ]]; then
        candidates=$control_port
    else
        candidates=$(seq "$base_port" $((base_port + 60)))
    fi
    for candidate in $candidates; do
        if [[ -n "$selected_failpoint" ]]; then
            MINITUN_TEST_FAILPOINT="$selected_failpoint" "$server_bin" --foreground \
                --listen "127.0.0.1:$candidate" \
                --tls-cert "$runtime_dir/server.crt" \
                --tls-key "$runtime_dir/server.key" \
                --clients-config "$runtime_dir/clients.json" \
                --heartbeat-interval 2 \
                --heartbeat-timeout 8 \
                --io-threads 2 >>"$runtime_dir/server.log" 2>&1 &
        else
            "$server_bin" --foreground \
                --listen "127.0.0.1:$candidate" \
                --tls-cert "$runtime_dir/server.crt" \
                --tls-key "$runtime_dir/server.key" \
                --clients-config "$runtime_dir/clients.json" \
                --heartbeat-interval 2 \
                --heartbeat-timeout 8 \
                --io-threads 2 >>"$runtime_dir/server.log" 2>&1 &
        fi
        server_pid=$!
        sleep 0.12
        if kill -0 "$server_pid" 2>/dev/null; then
            control_port=$candidate
            return
        fi
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    done
    sed -n '1,240p' "$runtime_dir/server.log" >&2
    exit 1
}

wait_for_injected_exit() {
    local process_id=$1
    for _ in $(seq 1 240); do
        if ! kill -0 "$process_id" 2>/dev/null; then
            set +e
            wait "$process_id"
            local status=$?
            set -e
            if [[ $status -ne 86 ]]; then
                printf 'failpoint process exited with %s instead of 86\n' "$status" >&2
                exit 1
            fi
            return
        fi
        sleep 0.05
    done
    printf 'failpoint %s did not terminate its process\n' "$failpoint" >&2
    exit 1
}

wait_tunnel_active() {
    local output="$runtime_dir/tunnel.json"
    for _ in $(seq 1 300); do
        if "$minitun_bin" --socket "$socket_path" tun inspect faulted --json \
                >"$output" 2>/dev/null &&
            python3 - "$output" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    tunnel = json.load(stream)
if tunnel["actual_state"] != "active" or tunnel["server_actual_state"] != "online":
    raise SystemExit(1)
PY
        then
            return
        fi
        sleep 0.1
    done
    cat "$output" >&2 || true
    sed -n '1,260p' "$runtime_dir/minitund.log" >&2
    sed -n '1,260p' "$runtime_dir/server.log" >&2
    exit 1
}

daemon_failpoint=
server_failpoint=
if [[ "$victim" == daemon ]]; then
    daemon_failpoint=$failpoint
else
    server_failpoint=$failpoint
fi

start_daemon "$daemon_failpoint"
bash "$integration_dir/write_client_policy.sh" "$minitun_bin" "$socket_path" \
    "$runtime_dir/clients.json" "$runtime_dir/token" >/dev/null
start_server "$server_failpoint"
"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" --name primary \
    >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary 9 "$remote_port" --name faulted \
    >/dev/null

if [[ "$victim" == daemon ]]; then
    wait_for_injected_exit "$daemon_pid"
    daemon_pid=
    start_daemon
else
    wait_for_injected_exit "$server_pid"
    server_pid=
    start_server
fi

wait_tunnel_active
python3 - "$remote_port" <<'PY'
import socket
import sys

connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1)
connection.close()
PY

if grep -F "$token" "$runtime_dir"/*.log; then
    printf 'authentication token leaked into fault-injection logs\n' >&2
    exit 1
fi

echo "fault injection reconciliation passed: $failpoint"
