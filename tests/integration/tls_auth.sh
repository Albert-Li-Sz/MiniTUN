#!/usr/bin/env bash
set -euo pipefail

server_bin=${1:?missing minitun-server binary}
client_bin=${2:?missing TLS test client binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-tls.XXXXXX")
server_pid=

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$runtime_dir"
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost \
    -keyout "$runtime_dir/server.key" \
    -out "$runtime_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/server.key"

token='stage-six-integration-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"
printf '%s\n' 'wrong-token' >"$runtime_dir/wrong-token"
chmod 0600 "$runtime_dir/wrong-token"

base_port=$((24000 + ($$ % 10000)))
for offset in $(seq 0 20); do
    port=$((base_port + offset))
    "$server_bin" \
        --foreground \
        --listen "127.0.0.1:$port" \
        --tls-cert "$runtime_dir/server.crt" \
        --tls-key "$runtime_dir/server.key" \
        --token-file "$runtime_dir/token" \
        --heartbeat-interval 1 \
        --heartbeat-timeout 3 \
        --io-threads 2 \
        >"$runtime_dir/server.log" 2>&1 &
    server_pid=$!
    sleep 0.1
    if kill -0 "$server_pid" 2>/dev/null; then
        break
    fi
    wait "$server_pid" 2>/dev/null || true
    server_pid=
done

if [[ -z "$server_pid" ]]; then
    echo 'TLS integration server did not start' >&2
    exit 1
fi

"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    --token-file "$runtime_dir/token" \
    --heartbeat-count 1

"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    --token-file "$runtime_dir/wrong-token" \
    --expect-auth-failure \
    --heartbeat-count 0

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=untrusted \
    -keyout "$runtime_dir/untrusted.key" \
    -out "$runtime_dir/untrusted.crt" >/dev/null 2>&1
if "$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/untrusted.crt" \
    --token-file "$runtime_dir/token" \
    --heartbeat-count 0; then
    echo 'untrusted TLS certificate was accepted' >&2
    exit 1
fi

if rg -F "$token" "$runtime_dir/server.log"; then
    echo 'authentication Token leaked into server logs' >&2
    exit 1
fi

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=

echo 'TLS authentication integration passed'
