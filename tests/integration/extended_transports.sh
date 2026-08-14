#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${1:?missing minitun binary}
minitund_bin=${2:?missing minitund binary}
server_bin=${3:?missing minitun-server binary}
p2p_bin=${4:?missing minitun-p2p binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-extended.XXXXXX")
integration_dir=$(cd "$(dirname "$0")" && pwd -P)
socket_path="$runtime_dir/minitun.sock"
daemon_pid=
server_pid=
echo_pid=
p2p_pid=

cleanup() {
    for process_id in "$p2p_pid" "$daemon_pid" "$server_pid" "$echo_pid"; do
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
token='extended-transports-test-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"

read -r control_port tcp_port udp_port udp_remote socks_remote p2p_remote p2p_local < <(
    python3 - <<'PY'
import socket

ports = []
for _ in range(7):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    ports.append(probe.getsockname()[1])
    probe.close()
print(*ports)
PY
)

python3 - "$tcp_port" "$udp_port" <<'PY' &
import signal
import socket
import sys
import threading

tcp_port, udp_port = map(int, sys.argv[1:])
stopping = threading.Event()

def stop(_signum, _frame):
    stopping.set()

def handle_tcp(connection):
    try:
        while True:
            chunk = connection.recv(16384)
            if not chunk:
                break
            connection.sendall(chunk)
    except OSError:
        pass
    finally:
        connection.close()

def serve_tcp():
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", tcp_port))
    listener.listen()
    listener.settimeout(0.1)
    while not stopping.is_set():
        try:
            connection, _ = listener.accept()
        except TimeoutError:
            continue
        threading.Thread(target=handle_tcp, args=(connection,), daemon=True).start()
    listener.close()

def serve_udp():
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(("127.0.0.1", udp_port))
    listener.settimeout(0.1)
    while not stopping.is_set():
        try:
            payload, peer = listener.recvfrom(65535)
        except TimeoutError:
            continue
        listener.sendto(payload, peer)
    listener.close()

signal.signal(signal.SIGTERM, stop)
tcp_thread = threading.Thread(target=serve_tcp, daemon=True)
udp_thread = threading.Thread(target=serve_udp, daemon=True)
tcp_thread.start()
udp_thread.start()
while not stopping.wait(0.1):
    pass
PY
echo_pid=$!

"$minitund_bin" --foreground \
    --socket "$socket_path" \
    --database "$runtime_dir/state.db" \
    --credentials "$runtime_dir/credentials.db" \
    --tls-ca "$runtime_dir/server.crt" \
    --io-threads 4 \
    >>"$runtime_dir/minitund.log" 2>&1 &
daemon_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$socket_path" ]] && break
    kill -0 "$daemon_pid" 2>/dev/null
    sleep 0.05
done
[[ -S "$socket_path" ]]

bash "$integration_dir/write_client_policy.sh" "$minitun_bin" "$socket_path" \
    "$runtime_dir/clients.json" "$runtime_dir/token" >/dev/null

"$server_bin" --foreground \
    --listen "127.0.0.1:$control_port" \
    --tls-cert "$runtime_dir/server.crt" \
    --tls-key "$runtime_dir/server.key" \
    --clients-config "$runtime_dir/clients.json" \
    --heartbeat-interval 1 \
    --heartbeat-timeout 3 \
    --io-threads 4 \
    >>"$runtime_dir/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do
    if python3 - "$control_port" <<'PY' 2>/dev/null
import socket
import sys
probe = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.1)
probe.close()
PY
    then
        break
    fi
    kill -0 "$server_pid" 2>/dev/null
    sleep 0.05
done

"$minitun_bin" --socket "$socket_path" server add "localhost:$control_port" \
    --name primary >/dev/null
printf '%s\n' "$token" |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary "$udp_port" "$udp_remote" \
    --name udp-echo --protocol udp >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary 1 "$socks_remote" \
    --name socks --protocol socks5 --remote-host 127.0.0.1 >/dev/null
"$minitun_bin" --socket "$socket_path" tun add primary "$tcp_port" "$p2p_remote" \
    --name p2p-echo --protocol p2p >/dev/null

wait_tunnel() {
    local name=$1
    for _ in $(seq 1 160); do
        if "$minitun_bin" --socket "$socket_path" tun inspect "$name" --json 2>/dev/null |
            python3 -c "import json,sys; raise SystemExit(json.load(sys.stdin)['actual_state'] != 'active')"
        then
            return
        fi
        sleep 0.1
    done
    sed -n '1,240p' "$runtime_dir/minitund.log" >&2
    return 1
}
wait_tunnel udp-echo
wait_tunnel socks
wait_tunnel p2p-echo

python3 - "$udp_remote" <<'PY'
import socket
import sys

peer = ("127.0.0.1", int(sys.argv[1]))
connection = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
connection.settimeout(5)
for payload in (b"udp-one", bytes(range(256)) * 8, b""):
    connection.sendto(payload, peer)
    echoed, source = connection.recvfrom(65535)
    if source != peer or echoed != payload:
        raise SystemExit("UDP tunnel did not preserve the datagram")
connection.close()
PY

python3 - "$socks_remote" "$tcp_port" <<'PY'
import socket
import struct
import sys

socks_port, destination_port = map(int, sys.argv[1:])
connection = socket.create_connection(("127.0.0.1", socks_port), timeout=3)
connection.settimeout(5)
connection.sendall(b"\x05\x01\x00")
if connection.recv(2) != b"\x05\x00":
    raise SystemExit("SOCKS5 method negotiation failed")
connection.sendall(b"\x05\x01\x00\x01\x7f\x00\x00\x01" + struct.pack("!H", destination_port))
header = connection.recv(4)
if len(header) != 4 or header[:2] != b"\x05\x00":
    raise SystemExit("SOCKS5 CONNECT failed")
remaining = 6 if header[3] == 1 else 18
while remaining:
    chunk = connection.recv(remaining)
    if not chunk:
        raise SystemExit("SOCKS5 reply was truncated")
    remaining -= len(chunk)
payload = b"socks5-through-minitun"
connection.sendall(payload)
received = b""
while len(received) < len(payload):
    received += connection.recv(len(payload) - len(received))
if received != payload:
    raise SystemExit("SOCKS5 relay payload mismatch")
connection.close()
PY

run_p2p_round_trip() {
    local mode=$1
    local expected=$2
    : >"$runtime_dir/p2p.log"
    if [[ -n "$mode" ]]; then
        "$p2p_bin" "127.0.0.1:$p2p_remote" --listen "127.0.0.1:$p2p_local" \
            --negotiation-timeout 5 --direct-timeout 2 "$mode" \
            >>"$runtime_dir/p2p.log" 2>&1 &
    else
        "$p2p_bin" "127.0.0.1:$p2p_remote" --listen "127.0.0.1:$p2p_local" \
            --negotiation-timeout 5 --direct-timeout 2 \
            >>"$runtime_dir/p2p.log" 2>&1 &
    fi
    p2p_pid=$!
    for _ in $(seq 1 100); do
        if python3 - "$p2p_local" <<'PY' 2>/dev/null
import socket
import sys
probe = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.1)
probe.close()
PY
        then
            break
        fi
        kill -0 "$p2p_pid" 2>/dev/null
        sleep 0.05
    done
    python3 - "$p2p_local" <<'PY'
import socket
import sys

payload = b"p2p-path-round-trip" * 1024
connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
connection.settimeout(8)
connection.sendall(payload)
received = bytearray()
while len(received) < len(payload):
    chunk = connection.recv(len(payload) - len(received))
    if not chunk:
        break
    received.extend(chunk)
if bytes(received) != payload:
    raise SystemExit("P2P payload mismatch")
connection.close()
PY
    for _ in $(seq 1 50); do
        grep -q "selected $expected path" "$runtime_dir/p2p.log" && break
        sleep 0.05
    done
    grep -q "selected $expected path" "$runtime_dir/p2p.log"
    kill -TERM "$p2p_pid"
    wait "$p2p_pid"
    p2p_pid=
    sleep 0.1
}

run_p2p_round_trip "" direct
run_p2p_round_trip "--relay-only" relay

printf 'extended transport integration passed\n'
