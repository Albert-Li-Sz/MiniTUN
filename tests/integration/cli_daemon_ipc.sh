#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <minitun> <minitund>\n' "$0" >&2
    exit 2
fi

minitun_bin=$1
minitund_bin=$2
tmp_root=${TMPDIR:-/tmp}
tmp_root=${tmp_root%/}
if [[ -z "$tmp_root" ]]; then
    tmp_root=/
fi
tmp_root=$(cd "$tmp_root" && pwd -P)
runtime_dir=$(mktemp -d "${tmp_root%/}/minitun-ipc.XXXXXX")
socket_path="$runtime_dir/minitun.sock"
daemon_log="$runtime_dir/minitund.log"
daemon_pid=
client_pids=()

cleanup() {
    for client_pid in "${client_pids[@]:-}"; do
        if [[ -z "$client_pid" ]]; then
            continue
        fi
        if kill -0 "$client_pid" 2>/dev/null; then
            kill -TERM "$client_pid" 2>/dev/null || true
            wait "$client_pid" 2>/dev/null || true
        fi
    done
    if [[ -n "$daemon_pid" ]] && kill -0 "$daemon_pid" 2>/dev/null; then
        kill -TERM "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$runtime_dir"
}
trap cleanup EXIT INT TERM

"$minitund_bin" --socket "$socket_path" >"$daemon_log" 2>&1 &
daemon_pid=$!

for _ in $(seq 1 100); do
    if [[ -S "$socket_path" ]]; then
        break
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
        printf 'minitund exited before creating its IPC socket\n' >&2
        sed -n '1,120p' "$daemon_log" >&2
        exit 1
    fi
    sleep 0.05
done

if [[ ! -S "$socket_path" ]]; then
    printf 'timed out waiting for the IPC socket\n' >&2
    sed -n '1,120p' "$daemon_log" >&2
    exit 1
fi

status_output=$("$minitun_bin" --socket "$socket_path" daemon status)
if [[ "$status_output" != *running* ]] || [[ "$status_output" != *"IPC version"* ]]; then
    printf 'unexpected daemon status output:\n%s\n' "$status_output" >&2
    exit 1
fi

for index in $(seq 1 12); do
    "$minitun_bin" --socket "$socket_path" daemon status \
        >"$runtime_dir/client-$index.out" 2>"$runtime_dir/client-$index.err" &
    client_pids+=("$!")
done
client_failed=0
for client_pid in "${client_pids[@]}"; do
    if ! wait "$client_pid"; then
        client_failed=1
    fi
done
client_pids=()
for index in $(seq 1 12); do
    if [[ $client_failed -ne 0 ]] ||
        ! grep -q 'State.*running' "$runtime_dir/client-$index.out" ||
        ! grep -q 'IPC version.*1' "$runtime_dir/client-$index.out" ||
        [[ -s "$runtime_dir/client-$index.err" ]]; then
        printf 'concurrent client %d returned an invalid result\n' "$index" >&2
        sed -n '1,40p' "$runtime_dir/client-$index.out" >&2
        sed -n '1,40p' "$runtime_dir/client-$index.err" >&2
        exit 1
    fi
done

kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=

if [[ -e "$socket_path" || -L "$socket_path" ]]; then
    printf 'minitund did not clean up its IPC socket\n' >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    printf 'python3 is required for the malformed IPC peer regression test\n' >&2
    exit 1
fi

python3 - "$socket_path" >"$runtime_dir/malformed-peer.log" 2>&1 <<'PY' &
import os
import socket
import struct
import sys


def receive_exact(connection, length):
    received = bytearray()
    while len(received) < length:
        chunk = connection.recv(length - len(received))
        if not chunk:
            raise RuntimeError("client closed before sending a complete request")
        received.extend(chunk)
    return bytes(received)


socket_path = sys.argv[1]
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.settimeout(5.0)
try:
    server.bind(socket_path)
    server.listen(1)
    connection, _ = server.accept()
    with connection:
        connection.settimeout(5.0)
        request_size = struct.unpack("!I", receive_exact(connection, 4))[0]
        if request_size == 0 or request_size > 1024 * 1024:
            raise RuntimeError("client sent an invalid frame length")
        receive_exact(connection, request_size)
        response = b'{"version":1}'
        connection.sendall(struct.pack("!I", len(response)) + response)
finally:
    server.close()
    try:
        os.unlink(socket_path)
    except FileNotFoundError:
        pass
PY
daemon_pid=$!

for _ in $(seq 1 100); do
    if [[ -S "$socket_path" ]]; then
        break
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
        printf 'malformed IPC peer exited before creating its socket\n' >&2
        sed -n '1,120p' "$runtime_dir/malformed-peer.log" >&2
        exit 1
    fi
    sleep 0.05
done

set +e
"$minitun_bin" --socket "$socket_path" daemon status \
    >"$runtime_dir/malformed.out" 2>"$runtime_dir/malformed.err"
malformed_status=$?
wait "$daemon_pid"
malformed_peer_status=$?
daemon_pid=
set -e
if [[ $malformed_peer_status -ne 0 ]]; then
    printf 'malformed IPC peer failed with status %d\n' "$malformed_peer_status" >&2
    sed -n '1,120p' "$runtime_dir/malformed-peer.log" >&2
    exit 1
fi
if [[ $malformed_status -ne 10 ]] ||
    ! grep -q 'IPC failure.*protocol_error' "$runtime_dir/malformed.err"; then
    printf 'malformed daemon response exit code was %d, expected protocol failure 10\n' \
        "$malformed_status" >&2
    sed -n '1,120p' "$runtime_dir/malformed.out" >&2
    sed -n '1,120p' "$runtime_dir/malformed.err" >&2
    exit 1
fi

set +e
"$minitun_bin" --socket "$socket_path" daemon status \
    >"$runtime_dir/unavailable.out" 2>"$runtime_dir/unavailable.err"
unavailable_status=$?
set -e
if [[ $unavailable_status -ne 3 ]]; then
    printf 'daemon-unavailable exit code was %d, expected 3\n' "$unavailable_status" >&2
    exit 1
fi

set +e
"$minitun_bin" --socket relative.sock daemon status \
    >"$runtime_dir/invalid-client.out" 2>"$runtime_dir/invalid-client.err"
invalid_client_status=$?
"$minitund_bin" --socket relative.sock \
    >"$runtime_dir/invalid-daemon.out" 2>"$runtime_dir/invalid-daemon.err"
invalid_daemon_status=$?
set -e
if [[ $invalid_client_status -ne 2 || $invalid_daemon_status -ne 2 ]]; then
    printf 'invalid socket path exit codes were client=%d daemon=%d, expected 2/2\n' \
        "$invalid_client_status" "$invalid_daemon_status" >&2
    exit 1
fi

printf 'CLI/daemon IPC integration passed\n'
