#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-manage.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
socket_path="$runtime_dir/minitun.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
admin_token='stage-ten-admin-token'
echo_pid=
daemon_pid=
server_pid=

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

printf '%s\n' "$admin_token" >"$runtime_dir/admin.token"
chmod 0600 "$runtime_dir/admin.token"

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost \
    -keyout "$runtime_dir/server.key" \
    -out "$runtime_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/server.key"
openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=MiniTun-Manage-Client-CA \
    -keyout "$runtime_dir/client-ca.key" \
    -out "$runtime_dir/client-ca.crt" >/dev/null 2>&1
openssl req -newkey rsa:2048 -sha256 -nodes \
    -subj /CN=minitun-manage-client \
    -keyout "$runtime_dir/client.key" \
    -out "$runtime_dir/client.csr" >/dev/null 2>&1
openssl x509 -req -sha256 -days 1 \
    -in "$runtime_dir/client.csr" \
    -CA "$runtime_dir/client-ca.crt" \
    -CAkey "$runtime_dir/client-ca.key" \
    -CAcreateserial \
    -extfile <(printf 'subjectAltName=DNS:manage-client.example\nextendedKeyUsage=clientAuth\n') \
    -out "$runtime_dir/client.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/client-ca.key" "$runtime_dir/client.key"

read -r control_port admin_port local_port remote_port < <(python3 - <<'PY'
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
        while True:
            chunk = connection.recv(16384)
            if not chunk:
                break
            connection.sendall(chunk)
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

# The server starts with a policy file containing no API-created client yet;
# the management API creates the client, credentials, and later rotates them.
printf '%s\n' 'seed-secret' >"$runtime_dir/seed.psk"
chmod 0600 "$runtime_dir/seed.psk"
python3 - "$runtime_dir/clients.json" <<'PY'
import json
import sys

with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(
        {
            "format_version": 1,
            "clients": [
                {
                    "client_id": "client_00000000000000000000000000000099",
                    "enabled": False,
                    "psk_file": "seed.psk",
                    "allowed_ports": ["1-65535"],
                    "max_tunnels": 1,
                    "max_connections": 1,
                    "max_idle_workers": 1,
                }
            ],
        },
        stream,
    )
    stream.write("\n")
PY
chmod 0640 "$runtime_dir/clients.json"

start_server() {
    "$server_bin" --foreground \
        --listen "127.0.0.1:$control_port" \
        --tls-cert "$runtime_dir/server.crt" \
        --tls-key "$runtime_dir/server.key" \
        --clients-config "$runtime_dir/clients.json" \
        --admin-listen "127.0.0.1:$admin_port" \
        --admin-token-file "$runtime_dir/admin.token" \
        --heartbeat-interval 1 \
        --heartbeat-timeout 3 \
        --relay-idle-timeout 2 \
        --io-threads 4 \
        >>"$runtime_dir/server.log" 2>&1 &
    server_pid=$!
    for _ in $(seq 1 100); do
        kill -0 "$server_pid" 2>/dev/null || return 1
        python3 - "$admin_port" <<'PY' 2>/dev/null && return
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

admin_call() {
    local method=$1
    local path=$2
    local body=${3-}
    python3 - "$admin_port" "$admin_token" "$method" "$path" "$body" <<'PY'
import json
import socket
import sys

port, token, method, path = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
body = sys.argv[5] if len(sys.argv) > 5 else None
request = (
    f"{method} {path} HTTP/1.1\r\n"
    f"Authorization: Bearer {token}\r\n"
    "Connection: close\r\n"
)
if body is not None:
    encoded = body.encode()
    request += f"Content-Length: {len(encoded)}\r\n"
else:
    encoded = b""
request += "\r\n"
connection = socket.create_connection(("127.0.0.1", port), timeout=0.5)
connection.sendall(request.encode() + encoded)
connection.shutdown(socket.SHUT_WR)
received = bytearray()
connection.settimeout(8)
while True:
    chunk = connection.recv(16384)
    if not chunk:
        break
    received.extend(chunk)
connection.close()
raw = bytes(received)
header, _, rest = raw.partition(b"\r\n\r\n")
status_line = header.split(b"\r\n", 1)[0].decode()
if not status_line.startswith("HTTP/1.1 200"):
    raise SystemExit(f"management {method} {path} failed: {status_line} {rest[:200]!r}")
print(rest.decode())
PY
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
    python3 - "$remote_port" <<'PY'
import hashlib
import socket
import sys

port = int(sys.argv[1])
payload = b"management-api-payload" * 64
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
    raise SystemExit("relay payload hash mismatch")
PY
}

start_server
start_daemon
managed_id=$("$minitun_bin" --socket "$socket_path" daemon identity --json |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["client_id"])')

# Health surface is authenticated like every /v1/* endpoint when a token exists.
python3 - "$admin_port" <<'PY'
import socket
import sys

connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.5)
connection.sendall(b"GET /v1/health HTTP/1.1\r\nConnection: close\r\n\r\n")
data = connection.recv(512)
connection.close()
if not data.startswith(b"HTTP/1.1 401"):
    raise SystemExit("unauthenticated /v1/health was not rejected")
PY

health=$(admin_call GET /v1/health)
python3 - "$health" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
if document.get("status") != "ok" or not document.get("server_id"):
    raise SystemExit("health payload is incomplete")
PY

# Create a client policy through the API; the generated PSK is returned once.
created=$(admin_call PUT "/v1/clients/$managed_id" '{
  "enabled": true,
  "allowed_ports": ["1-65535"],
  "max_tunnels": 8,
  "max_connections": 128,
  "max_idle_workers": 8
}')
psk=$(python3 - "$created" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
psk = document.get("psk")
if not psk or len(psk) != 64:
    raise SystemExit("create response did not return a 64-character PSK")
print(psk)
PY
)

python3 - "$admin_port" "$admin_token" "$managed_id" <<'PY'
import json
import socket
import sys

port, token, managed_id = int(sys.argv[1]), sys.argv[2], sys.argv[3]
request = (
    "GET /v1/clients HTTP/1.1\r\n"
    f"Authorization: Bearer {token}\r\n"
    "Connection: close\r\n\r\n"
)
connection = socket.create_connection(("127.0.0.1", port), timeout=0.5)
connection.sendall(request.encode())
received = bytearray()
while True:
    chunk = connection.recv(16384)
    if not chunk:
        break
    received.extend(chunk)
connection.close()
_, _, rest = bytes(received).partition(b"\r\n\r\n")
document = json.loads(rest)
summary = next(
    (entry for entry in document["clients"]
     if entry["client_id"] == sys.argv[3]),
    None,
)
if summary is None:
    raise SystemExit("client list does not reflect the created policy")
if "psk" in summary or summary.get("rotation_active"):
    raise SystemExit("client summary leaked or misreported rotation state")
PY

# The daemon authenticates with the PSK the API generated.
"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" --name primary \
    >/dev/null
"$minitun_bin" --socket "$socket_path" server update primary \
    --client-cert "$runtime_dir/client.crt" \
    --client-key "$runtime_dir/client.key" >/dev/null
printf '%s\n' "$psk" |
    "$minitun_bin" --socket "$socket_path" server login primary --psk-stdin >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary "$local_port" "$remote_port" \
    --name managed >/dev/null
wait_tunnel managed active
round_trip

# Rotate the PSK; the predecessor stays valid for the grace window so the
# running session keeps working while the client is re-provisioned.
rotated=$(admin_call POST "/v1/clients/$managed_id/rotate-psk" \
    '{"grace_seconds": 300}')
new_psk=$(python3 - "$rotated" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
psk = document.get("psk")
expiry = document.get("previous_psk_expires_at")
if not psk or len(psk) != 64:
    raise SystemExit("rotation response did not return the new PSK")
if not isinstance(expiry, int) or expiry <= 0:
    raise SystemExit("rotation response did not return an expiry")
print(psk)
PY
)
[[ "$new_psk" != "$psk" ]]

# The existing session was established with the old PSK; during the grace
# window relays keep working.
round_trip

# Re-provision the daemon with the new PSK and confirm the session returns.
printf '%s\n' "$new_psk" |
    "$minitun_bin" --socket "$socket_path" server login primary --psk-stdin >/dev/null
wait_tunnel managed active
round_trip

# The rotation state is visible in the summary without exposing secrets.
python3 - "$admin_port" "$admin_token" "$managed_id" <<'PY'
import json
import socket
import sys

port, token, managed_id = int(sys.argv[1]), sys.argv[2], sys.argv[3]
request = (
    f"GET /v1/clients/{managed_id} HTTP/1.1\r\n"
    f"Authorization: Bearer {token}\r\n"
    "Connection: close\r\n\r\n"
)
connection = socket.create_connection(("127.0.0.1", port), timeout=0.5)
connection.sendall(request.encode())
received = bytearray()
while True:
    chunk = connection.recv(16384)
    if not chunk:
        break
    received.extend(chunk)
connection.close()
_, _, rest = bytes(received).partition(b"\r\n\r\n")
summary = json.loads(rest)["client"]
if not summary.get("rotation_active"):
    raise SystemExit("rotation state is not reported")
if "psk" in summary:
    raise SystemExit("client summary leaked a PSK")
PY

# Deleting a client succeeds while another remains, and deleting the final
# client is rejected by the policy file invariant.
admin_call DELETE /v1/clients/client_00000000000000000000000000000099 >/dev/null
python3 - "$admin_port" "$admin_token" "$managed_id" <<'PY'
import socket
import sys

port, token, managed_id = int(sys.argv[1]), sys.argv[2], sys.argv[3]
request = (
    f"DELETE /v1/clients/{managed_id} HTTP/1.1\r\n"
    f"Authorization: Bearer {token}\r\n"
    "Connection: close\r\n\r\n"
)
connection = socket.create_connection(("127.0.0.1", port), timeout=0.5)
connection.sendall(request.encode())
received = bytearray()
while True:
    chunk = connection.recv(16384)
    if not chunk:
        break
    received.extend(chunk)
connection.close()
if not bytes(received).startswith(b"HTTP/1.1 400"):
    raise SystemExit("deleting the last client was not rejected")
PY

if grep -F "$psk" "$runtime_dir"/*.log 2>/dev/null; then
    printf 'PSK leaked into management or relay logs\n' >&2
    exit 1
fi

echo 'management API integration passed'
