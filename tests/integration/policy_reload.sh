#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-policy-reload.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
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

old_psk='policy-reload-old-private-value'
new_psk='policy-reload-new-private-value'
printf '%s\n' "$old_psk" >"$runtime_dir/old.psk"
printf '%s\n' "$new_psk" >"$runtime_dir/new.psk"
chmod 0600 "$runtime_dir/old.psk" "$runtime_dir/new.psk"

read -r control_port local_port remote_port < <(python3 - <<'PY'
import socket

sockets = []
for _ in range(3):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    sockets.append(probe)
print(*(probe.getsockname()[1] for probe in sockets))
for probe in sockets:
    probe.close()
PY
)

python3 - "$local_port" <<'PY' >"$runtime_dir/echo.log" 2>&1 &
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
            data = connection.recv(16384)
            if not data:
                break
            connection.sendall(data)
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
    kill -0 "$daemon_pid" 2>/dev/null || {
        sed -n '1,240p' "$runtime_dir/minitund.log" >&2
        exit 1
    }
    sleep 0.05
done
[[ -S "$socket_path" ]]

bash "$integration_dir/write_client_policy.sh" "$minitun_bin" "$socket_path" \
    "$runtime_dir/clients.json" "$runtime_dir/old.psk" >/dev/null

"$server_bin" --foreground \
    --listen "127.0.0.1:$control_port" \
    --tls-cert "$runtime_dir/server.crt" \
    --tls-key "$runtime_dir/server.key" \
    --clients-config "$runtime_dir/clients.json" \
    --heartbeat-interval 1 \
    --heartbeat-timeout 3 \
    --shutdown-timeout 1 \
    --min-idle-workers 1 \
    --io-threads 4 \
    >"$runtime_dir/server.log" 2>&1 &
server_pid=$!

for _ in $(seq 1 100); do
    if python3 - "$control_port" <<'PY' 2>/dev/null
import socket
import sys

with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2):
    pass
PY
    then
        break
    fi
    kill -0 "$server_pid" 2>/dev/null || {
        sed -n '1,240p' "$runtime_dir/server.log" >&2
        exit 1
    }
    sleep 0.05
done

"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" --name primary \
    >/dev/null
printf '%s\n' "$old_psk" |
    "$minitun_bin" --socket "$socket_path" server login primary --psk-stdin >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary "$local_port" "$remote_port" \
    --name relay >/dev/null

wait_tunnel() {
    local expected=$1
    for _ in $(seq 1 200); do
        if "$minitun_bin" --socket "$socket_path" tun inspect relay --json 2>/dev/null |
            python3 -c "import json,sys; raise SystemExit(json.load(sys.stdin)['actual_state'] != '$expected')"
        then
            return
        fi
        sleep 0.05
    done
    sed -n '1,280p' "$runtime_dir/minitund.log" >&2
    sed -n '1,280p' "$runtime_dir/server.log" >&2
    return 1
}

wait_tunnel active

# Rotate the client's effective policy while a relay is active. The listener
# must stop accepting immediately, the relay gets the configured one-second
# drain window, and then it is forcibly closed.
python3 - "$remote_port" "$server_pid" "$runtime_dir/clients.json" "$runtime_dir/new.psk" <<'PY'
import json
import os
import signal
import socket
import sys
import time

remote_port = int(sys.argv[1])
server_pid = int(sys.argv[2])
policy_path = sys.argv[3]
new_psk_path = os.path.abspath(sys.argv[4])


def receive_exact(connection, size):
    received = bytearray()
    while len(received) < size:
        chunk = connection.recv(size - len(received))
        if not chunk:
            raise RuntimeError("active relay closed before its drain window")
        received.extend(chunk)
    return bytes(received)


connection = socket.create_connection(("127.0.0.1", remote_port), timeout=1)
connection.settimeout(1)
before = b"before-policy-rotation"
connection.sendall(before)
if receive_exact(connection, len(before)) != before:
    raise SystemExit("pre-rotation relay payload mismatch")

with open(policy_path, encoding="utf-8") as stream:
    document = json.load(stream)
document["clients"][0]["psk_file"] = new_psk_path
temporary = policy_path + ".rotation.tmp"
with open(temporary, "w", encoding="utf-8") as stream:
    json.dump(document, stream, separators=(",", ":"), sort_keys=True)
    stream.write("\n")
os.chmod(temporary, 0o640)
os.replace(temporary, policy_path)
os.kill(server_pid, signal.SIGHUP)

time.sleep(0.2)
during = b"during-policy-drain"
connection.sendall(during)
if receive_exact(connection, len(during)) != during:
    raise SystemExit("relay payload mismatch during drain window")

deadline = time.monotonic() + 1.0
listener_stopped = False
while time.monotonic() < deadline:
    try:
        candidate = socket.create_connection(("127.0.0.1", remote_port), timeout=0.1)
        candidate.settimeout(0.2)
        candidate.sendall(b"new-traffic")
        if candidate.recv(1) == b"":
            listener_stopped = True
        candidate.close()
    except OSError:
        listener_stopped = True
    if listener_stopped:
        break
    time.sleep(0.05)
if not listener_stopped:
    raise SystemExit("policy reload continued to accept new relay traffic")

connection.settimeout(0.25)
deadline = time.monotonic() + 2.5
closed = False
while time.monotonic() < deadline:
    try:
        connection.sendall(b"drain-check")
        if connection.recv(32) == b"":
            closed = True
            break
    except (BrokenPipeError, ConnectionResetError, OSError):
        closed = True
        break
    time.sleep(0.05)
connection.close()
if not closed:
    raise SystemExit("active relay exceeded the bounded policy drain window")
PY

printf '%s\n' "$new_psk" |
    "$minitun_bin" --socket "$socket_path" server login primary --psk-stdin >/dev/null
wait_tunnel active

python3 - "$remote_port" <<'PY'
import socket
import sys

payload = b"relay-after-policy-rotation"
with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1) as connection:
    connection.settimeout(3)
    connection.sendall(payload)
    received = bytearray()
    while len(received) < len(payload):
        chunk = connection.recv(len(payload) - len(received))
        if not chunk:
            raise SystemExit("relay closed after policy rotation")
        received.extend(chunk)
if bytes(received) != payload:
    raise SystemExit("post-rotation relay payload mismatch")
PY

if grep -F -e "$old_psk" -e "$new_psk" "$runtime_dir"/*.log; then
    printf 'client PSK leaked into logs\n' >&2
    exit 1
fi

echo 'client policy reload integration passed'
