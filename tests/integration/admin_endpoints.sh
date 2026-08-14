#!/usr/bin/env bash
set -euo pipefail

minitund_bin=${1:?missing minitund binary}
server_bin=${2:?missing minitun-server binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-admin.XXXXXX")
daemon_pid=
server_pid=

cleanup() {
    for process_id in "$daemon_pid" "$server_pid"; do
        if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
            kill -TERM "$process_id" 2>/dev/null || true
            wait "$process_id" 2>/dev/null || true
        fi
    done
    rm -rf "$runtime_dir"
}
trap cleanup EXIT

read -r control_port daemon_admin_port server_admin_port < <(python3 - <<'PY'
import socket

ports = []
for _ in range(3):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    ports.append(probe.getsockname()[1])
    probe.close()
print(*ports)
PY
)

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost \
    -keyout "$runtime_dir/server.key" \
    -out "$runtime_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/server.key"
printf '%s\n' 'admin-endpoint-policy-secret' >"$runtime_dir/client.psk"
chmod 0600 "$runtime_dir/client.psk"
cat >"$runtime_dir/clients.json" <<'JSON'
{"format_version":1,"clients":[{"client_id":"client_00000000000000000000000000000001","enabled":true,"psk_file":"client.psk","allowed_ports":["1024-65535"],"max_tunnels":4,"max_connections":4,"max_idle_workers":2}]}
JSON
chmod 0640 "$runtime_dir/clients.json"

"$minitund_bin" --foreground \
    --socket "$runtime_dir/minitun.sock" \
    --database "$runtime_dir/state.db" \
    --credentials "$runtime_dir/credentials.db" \
    --admin-listen "127.0.0.1:$daemon_admin_port" \
    --io-threads 2 >"$runtime_dir/minitund.log" 2>&1 &
daemon_pid=$!

"$server_bin" --foreground \
    --listen "127.0.0.1:$control_port" \
    --tls-cert "$runtime_dir/server.crt" \
    --tls-key "$runtime_dir/server.key" \
    --clients-config "$runtime_dir/clients.json" \
    --admin-listen "127.0.0.1:$server_admin_port" \
    --io-threads 2 >"$runtime_dir/server.log" 2>&1 &
server_pid=$!

http_check() {
    local port=$1
    local method=$2
    local path=$3
    local expected_status=$4
    local expected_text=${5:-}
    python3 - "$port" "$method" "$path" "$expected_status" "$expected_text" <<'PY'
import socket
import sys
import time

port, method, path, expected_status, expected_text = sys.argv[1:]
last_error = None
for _ in range(100):
    try:
        connection = socket.create_connection(("127.0.0.1", int(port)), timeout=0.2)
        connection.settimeout(1)
        request = f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode()
        connection.sendall(request)
        response = bytearray()
        while True:
            block = connection.recv(4096)
            if not block:
                break
            response.extend(block)
        connection.close()
        header, body = bytes(response).split(b"\r\n\r\n", 1)
        status = header.split(b"\r\n", 1)[0].decode()
        if f" {expected_status} " not in status:
            raise RuntimeError(f"unexpected status: {status}")
        if method == "HEAD" and body:
            raise RuntimeError("HEAD response contained a body")
        if expected_text and expected_text.encode() not in response:
            raise RuntimeError("expected response text is absent")
        raise SystemExit(0)
    except (OSError, RuntimeError, ValueError) as error:
        last_error = error
        time.sleep(0.03)
raise SystemExit(f"admin request failed: {last_error}")
PY
}

http_check "$daemon_admin_port" GET /healthz 200 $'ok\n'
http_check "$daemon_admin_port" HEAD /readyz 200
http_check "$daemon_admin_port" GET /metrics 200 'minitun_build_info{role="daemon"'
http_check "$server_admin_port" GET /healthz 200 $'ok\n'
http_check "$server_admin_port" GET /readyz 200 $'ready\n'
http_check "$server_admin_port" GET /metrics 200 'minitun_build_info{role="server"'
http_check "$server_admin_port" HEAD /metrics 405
http_check "$server_admin_port" POST /healthz 405

if grep -F 'admin-endpoint-policy-secret' "$runtime_dir"/*.log; then
    echo 'admin integration secret leaked into logs' >&2
    exit 1
fi

kill -TERM "$daemon_pid" "$server_pid"
wait "$daemon_pid"
daemon_pid=
wait "$server_pid"
server_pid=

echo 'admin endpoint integration passed'
