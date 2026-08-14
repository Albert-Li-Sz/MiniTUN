#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-proxy.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
socket_path="$runtime_dir/minitun.sock"
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
headers_path="$runtime_dir/headers.log"
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
openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=MiniTun-Proxy-Client-CA \
    -keyout "$runtime_dir/client-ca.key" \
    -out "$runtime_dir/client-ca.crt" >/dev/null 2>&1
openssl req -newkey rsa:2048 -sha256 -nodes \
    -subj /CN=minitun-proxy-client \
    -keyout "$runtime_dir/client.key" \
    -out "$runtime_dir/client.csr" >/dev/null 2>&1
openssl x509 -req -sha256 -days 1 \
    -in "$runtime_dir/client.csr" \
    -CA "$runtime_dir/client-ca.crt" \
    -CAkey "$runtime_dir/client-ca.key" \
    -CAcreateserial \
    -extfile <(printf 'subjectAltName=DNS:proxy-client.example\nextendedKeyUsage=clientAuth\n') \
    -out "$runtime_dir/client.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/client-ca.key" "$runtime_dir/client.key"
token='stage-ten-proxy-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

read -r control_port local_port remote_port remote_port_two failed_remote_port < <(python3 - <<'PY'
import socket

ports = []
for _ in range(5):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    ports.append(probe.getsockname()[1])
    probe.close()
print(*ports)
PY
)

# The local echo target records the first line of every connection: either the
# received PROXY protocol v1 header or the literal "none", then echoes the
# remaining bytes back to the public client.
python3 - "$local_port" "$headers_path" <<'PY' &
import re
import signal
import socket
import sys
import threading

listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", int(sys.argv[1])))
listener.listen()
listener.settimeout(0.2)
log_path = sys.argv[2]
stopping = threading.Event()
log_lock = threading.Lock()
header_pattern = re.compile(rb"^PROXY (TCP4|TCP6) (\S+) (\S+) (\d+) (\d+)\r\n")


def stop(_signum, _frame):
    stopping.set()


def handle(connection):
    data = bytearray()
    try:
        connection.settimeout(8)
        while True:
            chunk = connection.recv(16384)
            if not chunk:
                break
            data.extend(chunk)
    except OSError:
        pass
    payload = bytes(data)
    header = b"none"
    match = header_pattern.match(payload)
    if match:
        family = match.group(1)
        source = match.group(2)
        if (family == b"TCP4") == (b":" in source):
            header = b"invalid-family"
        else:
            header = payload[: match.end()].rstrip(b"\r\n")
        payload = payload[match.end():]
    with log_lock:
        with open(log_path, "ab") as stream:
            stream.write(header + b"\n")
    try:
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
        --clients-config "$runtime_dir/clients.json" \
        --client-ca "$runtime_dir/client-ca.crt" \
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
    local port=$1
    python3 - "$port" <<'PY'
import hashlib
import socket
import sys

port = int(sys.argv[1])
payload = b"proxy-protocol-payload" * 64
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

read_headers() {
    if [[ -f "$headers_path" ]]; then
        cat "$headers_path"
    fi
}

assert_header() {
    local line=$1
    python3 - "$line" "$local_port" <<'PY'
import sys

line, local_port = sys.argv[1], sys.argv[2]
fields = line.split(" ")
if fields[0] != "PROXY" or fields[1] != "TCP4" or len(fields) != 6:
    raise SystemExit(f"malformed PROXY header: {line!r}")
source, destination, source_port, destination_port = fields[2:6]
if source != "127.0.0.1":
    raise SystemExit(f"unexpected PROXY source address: {source}")
if not source_port.isdigit() or not 1 <= int(source_port) <= 65535:
    raise SystemExit(f"unexpected PROXY source port: {source_port}")
if destination != "127.0.0.1":
    raise SystemExit(f"unexpected PROXY destination address: {destination}")
if destination_port != local_port:
    raise SystemExit(f"unexpected PROXY destination port: {destination_port}")
PY
}

start_daemon
client_id=$(bash "$integration_dir/write_client_policy.sh" "$minitun_bin" "$socket_path" \
    "$runtime_dir/clients.json" "$runtime_dir/token")
python3 - "$runtime_dir/clients.json" "$client_id" <<'PY'
import json
import os
import sys

path, expected_client_id = sys.argv[1:]
with open(path, encoding="utf-8") as stream:
    document = json.load(stream)
if document["clients"][0]["client_id"] != expected_client_id:
    raise SystemExit("client policy identity mismatch")
document["clients"][0]["certificate_san"] = "DNS:proxy-client.example"
temporary = path + ".cert.tmp"
with open(temporary, "w", encoding="utf-8") as stream:
    json.dump(document, stream, separators=(",", ":"), sort_keys=True)
    stream.write("\n")
os.chmod(temporary, 0o640)
os.replace(temporary, path)
PY
start_server
"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" --name primary \
    >/dev/null
"$minitun_bin" --socket "$socket_path" server update primary \
    --client-cert "$runtime_dir/client.crt" \
    --client-key "$runtime_dir/client.key" >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login primary --psk-stdin >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary "$local_port" "$remote_port" \
    --proxy-protocol --name proxy >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary "$local_port" "$remote_port_two" \
    --name plain >/dev/null
wait_tunnel proxy active
wait_tunnel plain active

# PROXY protocol is TCP-only and must be rejected for other tunnel modes.
if "$minitun_bin" --socket "$socket_path" tun add primary "$local_port" "$failed_remote_port" \
    --proxy-protocol --protocol udp --name rejected >/dev/null 2>&1; then
    echo "udp tunnel with --proxy-protocol was accepted" >&2
    exit 1
fi

# The enabled tunnel delivers a well-formed header; the plain tunnel does not.
"$minitun_bin" --socket "$socket_path" tun inspect proxy --json |
    python3 -c 'import json,sys; assert json.load(sys.stdin)["proxy_protocol"] is True' || exit 1
"$minitun_bin" --socket "$socket_path" tun inspect plain --json |
    python3 -c 'import json,sys; assert json.load(sys.stdin)["proxy_protocol"] is False' || exit 1
round_trip "$remote_port"
round_trip "$remote_port_two"

headers=()
while IFS= read -r line; do
    headers+=("$line")
done < <(read_headers)
[[ ${#headers[@]} -eq 2 ]] || { echo "expected two recorded connections" >&2; exit 1; }
assert_header "${headers[0]}"
[[ "${headers[1]}" == "none" ]] || { echo "plain tunnel leaked a PROXY header" >&2; exit 1; }

# toggling the flag through tun update must flip the behavior both ways.
"$minitun_bin" --socket "$socket_path" tun update proxy --no-proxy-protocol >/dev/null
wait_tunnel proxy active
"$minitun_bin" --socket "$socket_path" tun inspect proxy --json |
    python3 -c 'import json,sys; assert json.load(sys.stdin)["proxy_protocol"] is False' || exit 1
round_trip "$remote_port"
"$minitun_bin" --socket "$socket_path" tun update proxy --proxy-protocol >/dev/null
wait_tunnel proxy active
"$minitun_bin" --socket "$socket_path" tun inspect proxy --json |
    python3 -c 'import json,sys; assert json.load(sys.stdin)["proxy_protocol"] is True' || exit 1
round_trip "$remote_port"

headers=()
while IFS= read -r line; do
    headers+=("$line")
done < <(read_headers)
[[ ${#headers[@]} -eq 4 ]] || { echo "expected four recorded connections" >&2; exit 1; }
assert_header "${headers[0]}"
[[ "${headers[1]}" == "none" ]] || { echo "plain tunnel leaked a PROXY header" >&2; exit 1; }
[[ "${headers[2]}" == "none" ]] || { echo "disabled flag still emitted a PROXY header" >&2; exit 1; }
assert_header "${headers[3]}"

if grep -F "$token" "$runtime_dir"/*.log; then
    printf 'authentication token leaked into relay logs\n' >&2
    exit 1
fi

echo 'PROXY protocol integration passed'
