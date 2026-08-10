#!/usr/bin/env bash
set -euo pipefail

server_bin=${1:?missing minitun-server binary}
client_bin=${2:?missing TLS test client binary}

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-tls.XXXXXX")
server_pid=
goaway_client_pid=

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    if [[ -n "$goaway_client_pid" ]] && kill -0 "$goaway_client_pid" 2>/dev/null; then
        kill -TERM "$goaway_client_pid" 2>/dev/null || true
        wait "$goaway_client_pid" 2>/dev/null || true
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

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=MiniTun-Test-Client-CA \
    -keyout "$runtime_dir/client-ca.key" \
    -out "$runtime_dir/client-ca.crt" >/dev/null 2>&1
for identity in client wrong-client; do
    san=client.example
    if [[ "$identity" == wrong-client ]]; then
        san=wrong.example
    fi
    openssl req -newkey rsa:2048 -sha256 -nodes \
        -subj "/CN=$identity" \
        -keyout "$runtime_dir/$identity.key" \
        -out "$runtime_dir/$identity.csr" >/dev/null 2>&1
    openssl x509 -req -sha256 -days 1 \
        -in "$runtime_dir/$identity.csr" \
        -CA "$runtime_dir/client-ca.crt" \
        -CAkey "$runtime_dir/client-ca.key" \
        -CAcreateserial \
        -extfile <(printf 'subjectAltName=DNS:%s\nextendedKeyUsage=clientAuth\n' "$san") \
        -out "$runtime_dir/$identity.crt" >/dev/null 2>&1
    chmod 0600 "$runtime_dir/$identity.key"
done
chmod 0600 "$runtime_dir/client-ca.key"
client_identity=(--client-cert "$runtime_dir/client.crt" --client-key "$runtime_dir/client.key")

token='stage-six-integration-token'
rotated_token='stage-six-rotated-token'
printf '%s\n' "$token" >"$runtime_dir/token"
chmod 0600 "$runtime_dir/token"
printf '%s\n' "$token" >"$runtime_dir/old-token"
chmod 0600 "$runtime_dir/old-token"
printf '%s\n' 'wrong-token' >"$runtime_dir/wrong-token"
chmod 0600 "$runtime_dir/wrong-token"
cat >"$runtime_dir/clients.json" <<'JSON'
{"format_version":1,"clients":[{"client_id":"client_00000000000000000000000000000001","enabled":true,"psk_file":"token","certificate_san":"DNS:client.example","allowed_ports":["1024-65535"],"max_tunnels":128,"max_connections":10000,"max_idle_workers":32}]}
JSON
chmod 0640 "$runtime_dir/clients.json"

base_port=$((24000 + ($$ % 10000)))
for offset in $(seq 0 20); do
    port=$((base_port + offset))
    "$server_bin" \
        --foreground \
        --listen "127.0.0.1:$port" \
        --tls-cert "$runtime_dir/server.crt" \
        --tls-key "$runtime_dir/server.key" \
        --clients-config "$runtime_dir/clients.json" \
        --client-ca "$runtime_dir/client-ca.crt" \
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
    "${client_identity[@]}" \
    --token-file "$runtime_dir/token" \
    --heartbeat-count 1

"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    "${client_identity[@]}" \
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
    "${client_identity[@]}" \
    --token-file "$runtime_dir/token" \
    --heartbeat-count 0; then
    echo 'untrusted TLS certificate was accepted' >&2
    exit 1
fi

"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    --token-file "$runtime_dir/token" \
    --expect-auth-failure \
    --heartbeat-count 0

"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    --client-cert "$runtime_dir/wrong-client.crt" \
    --client-key "$runtime_dir/wrong-client.key" \
    --token-file "$runtime_dir/token" \
    --expect-auth-failure \
    --heartbeat-count 0

authenticated_before=$(grep -cF 'remote client authenticated' "$runtime_dir/server.log" || true)
"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    "${client_identity[@]}" \
    --token-file "$runtime_dir/token" \
    --expect-goaway \
    --heartbeat-count 0 &
goaway_client_pid=$!
for _ in $(seq 1 100); do
    authenticated_now=$(grep -cF 'remote client authenticated' "$runtime_dir/server.log" || true)
    if ((authenticated_now > authenticated_before)); then
        break
    fi
    kill -0 "$goaway_client_pid" 2>/dev/null || break
    sleep 0.02
done

printf '%s\n' "$rotated_token" >"$runtime_dir/token.next"
chmod 0600 "$runtime_dir/token.next"
mv "$runtime_dir/token.next" "$runtime_dir/token"
kill -HUP "$server_pid"
wait "$goaway_client_pid"
goaway_client_pid=
kill -0 "$server_pid"

"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    "${client_identity[@]}" \
    --token-file "$runtime_dir/old-token" \
    --expect-auth-failure \
    --heartbeat-count 0

"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    "${client_identity[@]}" \
    --token-file "$runtime_dir/token" \
    --heartbeat-count 1

if grep -F -e "$token" -e "$rotated_token" "$runtime_dir/server.log"; then
    echo 'authentication Token leaked into server logs' >&2
    exit 1
fi

"$client_bin" \
    --endpoint "127.0.0.1:$port" \
    --server-name localhost \
    --ca-cert "$runtime_dir/server.crt" \
    "${client_identity[@]}" \
    --token-file "$runtime_dir/token" \
    --expect-goaway \
    --heartbeat-count 0 &
goaway_client_pid=$!
sleep 0.2
kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
wait "$goaway_client_pid"
goaway_client_pid=

echo 'TLS authentication integration passed'
