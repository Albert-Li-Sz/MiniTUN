#!/usr/bin/env bash
# Verifies that minitun-server reports a startup warning when the configured
# connection ceiling cannot fit inside the process memory ceiling it can see.
#
# The ceiling is applied through setrlimit in a Python launcher rather than the
# shell's ulimit: macOS bash rejects lowering RLIMIT_AS/RLIMIT_DATA, while
# setrlimit works there and on Linux. Nothing is actually allocated against the
# limit, so the check stays cheap.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'usage: %s <minitun-server>\n' "$0" >&2
    exit 2
fi

server_bin=$1
runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-memory-budget.XXXXXX")
server_pid=
server_log="$runtime_dir/server.log"

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$runtime_dir"
}
trap cleanup EXIT INT TERM

for tool in openssl python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf '%s is required for the memory budget integration test\n' "$tool" >&2
        exit 1
    fi
done

# Sanitizer runtimes reserve huge shadow mappings and refuse to start under a
# tight RLIMIT_AS (ThreadSanitizer aborts with "setrlimit() failed"). The
# budget logic under test is independent of instrumentation, so skip there.
if [[ -n "${ASAN_OPTIONS:-}${TSAN_OPTIONS:-}${UBSAN_OPTIONS:-}" ]]; then
    printf 'sanitizer runtime options are set; skipping the address-space limit test\n' >&2
    exit 77
fi
if command -v nm >/dev/null 2>&1 &&
    nm "$server_bin" 2>/dev/null | grep -qiE "__(asan|tsan|ubsan|sanitizer)"; then
    printf 'server binary is sanitizer-instrumented; skipping\n' >&2
    exit 77
fi

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost \
    -keyout "$runtime_dir/server.key" \
    -out "$runtime_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/server.key"

umask 077
head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' >"$runtime_dir/client.psk"
cat >"$runtime_dir/clients.json" <<JSON
{
  "format_version": 1,
  "clients": [
    {
      "client_id": "client_00000000000000000000000000000001",
      "enabled": true,
      "psk_file": "$runtime_dir/client.psk",
      "allowed_ports": ["10000-10999"],
      "max_tunnels": 128,
      "max_connections": 10000,
      "max_idle_workers": 32
    }
  ]
}
JSON

cat >"$runtime_dir/launch.py" <<'PY'
import os
import resource
import sys

limit = 128 * 1024 * 1024
applied = 0
for name in ("RLIMIT_AS", "RLIMIT_DATA"):
    which = getattr(resource, name, None)
    if which is None:
        continue
    try:
        resource.setrlimit(which, (limit, limit))
        applied += 1
    except (ValueError, OSError):
        continue
if applied == 0:
    sys.exit(77)
os.execv(sys.argv[1], sys.argv[1:])
PY

# An intentionally tight 128 MiB ceiling cannot hold the default
# max-total-connections budget, so the warning must fire. The listener still
# needs a real port: Endpoint::parse rejects port 0, so probe for a free one.
free_port=$(python3 - <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
    probe.bind(("127.0.0.1", 0))
    print(probe.getsockname()[1])
PY
)

python3 "$runtime_dir/launch.py" "$server_bin" --foreground \
    --listen "127.0.0.1:${free_port}" \
    --tls-cert "$runtime_dir/server.crt" --tls-key "$runtime_dir/server.key" \
    --clients-config "$runtime_dir/clients.json" >"$server_log" 2>&1 &
server_pid=$!

for _ in $(seq 1 100); do
    if grep -q "may exceed the process memory limit" "$server_log"; then
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        status=0
        wait "$server_pid" 2>/dev/null || status=$?
        server_pid=
        if [[ $status -eq 77 ]]; then
            printf 'no adjustable memory limit is available; skipping\n' >&2
            exit 77
        fi
        printf 'minitun-server exited before reporting its memory budget\n' >&2
        cat "$server_log" >&2
        exit 1
    fi
    sleep 0.1
done

if ! grep -q "may exceed the process memory limit" "$server_log"; then
    printf 'minitun-server did not report the memory budget warning\n' >&2
    cat "$server_log" >&2
    exit 1
fi
if ! grep -q "max-total-connections" "$server_log"; then
    printf 'memory budget warning did not name the connection limit\n' >&2
    cat "$server_log" >&2
    exit 1
fi

printf 'memory budget integration passed\n'
