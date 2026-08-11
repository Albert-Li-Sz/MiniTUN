#!/usr/bin/env bash
set -euo pipefail

minitund_bin=${1:?missing minitund binary}
gui_bin=${2:?missing minitun-gui binary}
assets_dir=${3:?missing GUI assets directory}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-gui-test.XXXXXX")
socket_path="$runtime_dir/minitun.sock"
runtime_assets="$runtime_dir/assets"
daemon_pid=
gui_pid=

mkdir -p "$runtime_assets"
cp -R "$assets_dir/." "$runtime_assets/"
printf 'outside asset\n' >"$runtime_dir/outside.txt"
ln -s "$runtime_dir/outside.txt" "$runtime_assets/escape.txt"

cleanup() {
    for process_id in "$gui_pid" "$daemon_pid"; do
        if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
            kill -TERM "$process_id" 2>/dev/null || true
            wait "$process_id" 2>/dev/null || true
        fi
    done
    rm -rf "$runtime_dir"
}
trap cleanup EXIT INT TERM

port=$(python3 - <<'PY'
import socket
probe = socket.socket()
probe.bind(("127.0.0.1", 0))
print(probe.getsockname()[1])
probe.close()
PY
)

"$minitund_bin" --foreground --socket "$socket_path" \
    --database "$runtime_dir/state.db" \
    --credentials "$runtime_dir/credentials.db" \
    >>"$runtime_dir/minitund.log" 2>&1 &
daemon_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$socket_path" ]] && break
    kill -0 "$daemon_pid" 2>/dev/null
    sleep 0.05
done
[[ -S "$socket_path" ]]

"$gui_bin" --listen "127.0.0.1:$port" --socket "$socket_path" \
    --assets-dir "$runtime_assets" >>"$runtime_dir/gui.log" 2>&1 &
gui_pid=$!
for _ in $(seq 1 100); do
    if python3 - "$port" <<'PY' 2>/dev/null
import socket
import sys
probe = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.1)
probe.close()
PY
    then
        break
    fi
    kill -0 "$gui_pid" 2>/dev/null
    sleep 0.05
done

python3 - "$port" <<'PY'
import http.client
import json
import socket
import sys

port = int(sys.argv[1])

def request(method, path, body=None, headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request(method, path, body=body, headers=headers or {})
    response = connection.getresponse()
    payload = response.read()
    result = response.status, dict(response.getheaders()), payload
    connection.close()
    return result

status, headers, body = request("GET", "/")
assert status == 200, status
assert b"MiniTun" in body
assert "default-src 'self'" in headers["Content-Security-Policy"]
assert headers["X-Frame-Options"] == "DENY"
assert headers["X-Content-Type-Options"] == "nosniff"

status, headers, body = request("GET", "/api/v1/status")
assert status == 200, (status, body)
document = json.loads(body)
assert "status" in document or "servers" in document
assert headers["Cache-Control"] == "no-store"

evil_headers = {
    "Origin": "https://evil.example",
    "Content-Type": "application/json",
}
status, _, body = request("POST", "/api/v1/tunnels", "{}", evil_headers)
assert status == 403, (status, body)

missing_origin_headers = {"Content-Type": "application/json"}
status, _, body = request("POST", "/api/v1/tunnels", "{}", missing_origin_headers)
assert status == 403, (status, body)

same_origin_headers = {
    "Origin": f"http://127.0.0.1:{port}",
    "Content-Type": "application/json",
}
status, _, body = request("POST", "/api/v1/tunnels", "{}", same_origin_headers)
assert status == 400, (status, body)

status, _, _ = request("TRACE", "/")
assert status == 405, status

status, _, _ = request("GET", "/escape.txt")
assert status == 404, status

def raw_status(payload):
    connection = socket.create_connection(("127.0.0.1", port), timeout=3)
    connection.sendall(payload)
    response = bytearray()
    while b"\r\n" not in response:
        chunk = connection.recv(4096)
        if not chunk:
            break
        response.extend(chunk)
    connection.close()
    return int(response.split(b" ", 2)[1])

assert raw_status(
    f"GET /../../etc/passwd HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n\r\n".encode()
) == 400
assert raw_status(
    f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nHost: duplicate\r\n\r\n".encode()
) == 400
assert raw_status(b"GET / HTTP/1.1\r\nHost: attacker.example\r\n\r\n") == 403
PY

printf 'GUI console integration passed\n'
