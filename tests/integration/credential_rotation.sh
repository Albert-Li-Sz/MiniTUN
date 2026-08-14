#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/mt-cred.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
socket_path="$runtime_dir/d.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
daemon_pid=
server_pid=
proxy_pid=

cleanup() {
    for process_id in "$daemon_pid" "$proxy_pid" "$server_pid"; do
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
correct_token='credential-rotation-correct-token'
wrong_token='credential-rotation-obsolete-token'
printf '%s\n' "$correct_token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

read -r server_port proxy_port < <(python3 - <<'PY'
import socket

sockets = []
for _ in range(2):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    sockets.append(probe)
print(*(probe.getsockname()[1] for probe in sockets))
for probe in sockets:
    probe.close()
PY
)

"$minitund_bin" --foreground \
    --socket "$socket_path" \
    --database "$state_path" \
    --credentials "$credentials_path" \
    --tls-ca "$runtime_dir/server.crt" \
    --io-threads 4 \
    >"$runtime_dir/minitund.log" 2>&1 &
daemon_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$socket_path" ]] && break
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
        sed -n '1,240p' "$runtime_dir/minitund.log" >&2
        exit 1
    fi
    sleep 0.05
done
[[ -S "$socket_path" ]]

bash "$integration_dir/write_client_policy.sh" "$minitun_bin" "$socket_path" \
    "$runtime_dir/clients.json" "$runtime_dir/token" >/dev/null

"$server_bin" --foreground \
    --listen "127.0.0.1:$server_port" \
    --tls-cert "$runtime_dir/server.crt" \
    --tls-key "$runtime_dir/server.key" \
    --clients-config "$runtime_dir/clients.json" \
    --heartbeat-interval 1 \
    --heartbeat-timeout 3 \
    --min-idle-workers 0 \
    --io-threads 2 \
    >"$runtime_dir/server.log" 2>&1 &
server_pid=$!

server_ready=false
for _ in $(seq 1 100); do
    if python3 - "$server_port" <<'PY'
import socket
import sys

with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2):
    pass
PY
    then
        server_ready=true
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        sed -n '1,200p' "$runtime_dir/server.log" >&2
        exit 1
    fi
    sleep 0.05
done
[[ "$server_ready" == true ]]

python3 - "$proxy_port" "$server_port" <<'PY' \
    >"$runtime_dir/proxy.log" 2>&1 &
import signal
import socket
import sys
import threading
import time

listen_port = int(sys.argv[1])
target_port = int(sys.argv[2])
delay_seconds = 0.20
running = True


def stop(_signum, _frame):
    global running
    running = False
    listener.close()


def relay(source, destination):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            time.sleep(delay_seconds)
            destination.sendall(data)
    except OSError:
        pass
    finally:
        try:
            destination.shutdown(socket.SHUT_WR)
        except OSError:
            pass


listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", listen_port))
listener.listen()
signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

while running:
    try:
        client, _ = listener.accept()
    except OSError:
        if running:
            raise
        break
    try:
        upstream = socket.create_connection(("127.0.0.1", target_port), timeout=2)
    except OSError:
        client.close()
        continue
    threading.Thread(target=relay, args=(client, upstream), daemon=True).start()
    threading.Thread(target=relay, args=(upstream, client), daemon=True).start()
PY
proxy_pid=$!

for _ in $(seq 1 100); do
    if python3 - "$proxy_port" <<'PY'
import socket
import sys

with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2):
    pass
PY
    then
        break
    fi
    if ! kill -0 "$proxy_pid" 2>/dev/null; then
        sed -n '1,160p' "$runtime_dir/proxy.log" >&2
        exit 1
    fi
    sleep 0.05
done

"$minitun_bin" --socket "$socket_path" server add "localhost:$proxy_port" --name primary \
    >/dev/null
printf '%s\n' "$wrong_token" |
    "$minitun_bin" --socket "$socket_path" server login primary --psk-stdin >/dev/null

observed_authenticating=false
for _ in $(seq 1 300); do
    if "$minitun_bin" --socket "$socket_path" server inspect primary --json \
            >"$runtime_dir/server-state.json" 2>/dev/null &&
        python3 - "$runtime_dir/server-state.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    server = json.load(stream)
raise SystemExit(server["actual_state"] != "authenticating")
PY
    then
        observed_authenticating=true
        break
    fi
    sleep 0.02
done
if [[ "$observed_authenticating" != true ]]; then
    cat "$runtime_dir/server-state.json" >&2 2>/dev/null || true
    sed -n '1,240p' "$runtime_dir/minitund.log" >&2
    exit 1
fi

# Let the old session read its credential while the proxy still delays the
# HELLO/AUTH exchange, then rotate the token underneath that in-flight session.
sleep 0.10
printf '%s\n' "$correct_token" |
    "$minitun_bin" --socket "$socket_path" server login primary --psk-stdin >/dev/null

online=false
for _ in $(seq 1 300); do
    if "$minitun_bin" --socket "$socket_path" server inspect primary --json \
            >"$runtime_dir/server-state.json" 2>/dev/null &&
        python3 - "$runtime_dir/server-state.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    server = json.load(stream)
raise SystemExit(server["actual_state"] != "online")
PY
    then
        online=true
        break
    fi
    sleep 0.05
done
if [[ "$online" != true ]]; then
    cat "$runtime_dir/server-state.json" >&2 2>/dev/null || true
    sed -n '1,280p' "$runtime_dir/minitund.log" >&2
    sed -n '1,240p' "$runtime_dir/server.log" >&2
    sed -n '1,240p' "$runtime_dir/proxy.log" >&2
    exit 1
fi

if grep -F -e "$wrong_token" -e "$correct_token" "$runtime_dir"/*.log; then
    printf 'credential token leaked into logs\n' >&2
    exit 1
fi

echo 'credential rotation integration passed'
