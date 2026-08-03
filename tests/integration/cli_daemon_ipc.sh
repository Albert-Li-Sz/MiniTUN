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
state_path="$runtime_dir/state.db"
credentials_path="$runtime_dir/credentials.db"
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

if ! command -v python3 >/dev/null 2>&1; then
    printf 'python3 is required for the CLI/daemon integration test\n' >&2
    exit 1
fi

start_daemon() {
    "$minitund_bin" --foreground --socket "$socket_path" \
        --database "$state_path" --credentials "$credentials_path" \
        >>"$daemon_log" 2>&1 &
    daemon_pid=$!

    for _ in $(seq 1 100); do
        if [[ -S "$socket_path" ]]; then
            return
        fi
        if ! kill -0 "$daemon_pid" 2>/dev/null; then
            printf 'minitund exited before creating its IPC socket\n' >&2
            sed -n '1,160p' "$daemon_log" >&2
            exit 1
        fi
        sleep 0.05
    done

    printf 'timed out waiting for the IPC socket\n' >&2
    sed -n '1,160p' "$daemon_log" >&2
    exit 1
}

stop_daemon() {
    kill -TERM "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
    if [[ -e "$socket_path" || -L "$socket_path" ]]; then
        printf 'minitund did not clean up its IPC socket\n' >&2
        exit 1
    fi
}

: >"$daemon_log"
start_daemon

status_output=$("$minitun_bin" --socket "$socket_path" daemon status)
if [[ "$status_output" != *running* ]] || [[ "$status_output" != *"IPC version"* ]]; then
    printf 'unexpected daemon status output:\n%s\n' "$status_output" >&2
    exit 1
fi

"$minitun_bin" --socket "$socket_path" server add 127.0.0.1:1 --name primary \
    >"$runtime_dir/server-add.out" 2>"$runtime_dir/server-add.err"
if ! grep -q 'Status.*not_authenticated' "$runtime_dir/server-add.out" ||
    [[ -s "$runtime_dir/server-add.err" ]]; then
    printf 'server add returned an invalid result\n' >&2
    exit 1
fi

"$minitun_bin" --socket "$socket_path" server list --json \
    >"$runtime_dir/server-list.json" 2>"$runtime_dir/server-list.err"
server_id=$(python3 - "$runtime_dir/server-list.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    servers = json.load(stream)
assert len(servers) == 1
server = servers[0]
assert server["name"] == "primary"
assert server["endpoint"] == "127.0.0.1:1"
assert server["actual_state"] == "not_authenticated"
assert server["credential_configured"] is False
assert "credential_ref" not in server
print(server["id"])
PY
)

orphan_token=phase4-orphan-private-token
stop_daemon
python3 - "$credentials_path" "$server_id" "$orphan_token" <<'PY'
import sqlite3
import sys

connection = sqlite3.connect(sys.argv[1])
try:
    connection.execute(
        "INSERT INTO credentials(key, secret, updated_at) VALUES(?, ?, 0)",
        ("server/" + sys.argv[2], sys.argv[3].encode()),
    )
    connection.commit()
finally:
    connection.close()
PY
start_daemon
python3 - "$credentials_path" <<'PY'
import sqlite3
import sys

connection = sqlite3.connect(sys.argv[1])
try:
    assert connection.execute("SELECT COUNT(*) FROM credentials").fetchone()[0] == 0
finally:
    connection.close()
PY

stdin_token=phase4-stdin-private-token
printf '%s\n' "$stdin_token" |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin \
        >"$runtime_dir/login-stdin.out" 2>"$runtime_dir/login-stdin.err"
if ! grep -q 'Status.*disconnected' "$runtime_dir/login-stdin.out" ||
    grep -q "$stdin_token" "$runtime_dir/login-stdin.out" "$runtime_dir/login-stdin.err"; then
    printf 'token-stdin login returned an invalid or sensitive result\n' >&2
    exit 1
fi

set +e
python3 -c 'import sys; sys.stdout.write("x" * 65537 + "\n")' |
    "$minitun_bin" --socket "$socket_path" server login primary --token-stdin \
        >"$runtime_dir/login-oversized.out" 2>"$runtime_dir/login-oversized.err"
oversized_token_status=${PIPESTATUS[1]}
set -e
if [[ $oversized_token_status -ne 2 ]] ||
    ! grep -q 'outside its accepted byte-length' "$runtime_dir/login-oversized.err" ||
    [[ -s "$runtime_dir/login-oversized.out" ]]; then
    printf 'oversized token-stdin input was not rejected safely\n' >&2
    exit 1
fi

set +e
printf '%s\n' "$stdin_token" |
    "$minitun_bin" --socket "$socket_path" server login primary \
        >"$runtime_dir/login-no-flag.out" 2>"$runtime_dir/login-no-flag.err"
login_no_flag_status=$?
set -e
if [[ $login_no_flag_status -ne 2 ]]; then
    printf 'non-terminal login without --token-stdin returned %d, expected 2\n' \
        "$login_no_flag_status" >&2
    exit 1
fi

interactive_token=phase4-interactive-private-token
python3 - "$minitun_bin" "$socket_path" "$interactive_token" <<'PY'
import errno
import os
import pty
import select
import sys
import time

binary, socket_path, token = sys.argv[1:]
child_pid, descriptor = pty.fork()
if child_pid == 0:
    os.execv(binary, [binary, "--socket", socket_path, "server", "login", "primary"])

output = bytearray()
sent = False
finished = 0
wait_status = 0
deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    ready, _, _ = select.select([descriptor], [], [], 0.1)
    if ready:
        try:
            chunk = os.read(descriptor, 4096)
        except OSError as error:
            if error.errno == errno.EIO:
                break
            raise
        if not chunk:
            break
        output.extend(chunk)
        if not sent and b"Token: " in output:
            os.write(descriptor, token.encode() + b"\n")
            sent = True
    finished, wait_status = os.waitpid(child_pid, os.WNOHANG)
    if finished:
        break
else:
    os.kill(child_pid, 15)
    raise RuntimeError("interactive login timed out")

if not finished:
    _, wait_status = os.waitpid(child_pid, 0)
exit_code = os.waitstatus_to_exitcode(wait_status)
text = output.decode(errors="replace")
assert sent, text
assert exit_code == 0, (exit_code, text)
assert token not in text, text
assert "server credentials stored" in text.lower(), text
PY

"$minitun_bin" --socket "$socket_path" server inspect primary --json \
    >"$runtime_dir/server-inspect.json" 2>"$runtime_dir/server-inspect.err"
python3 - "$runtime_dir/server-inspect.json" "$server_id" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    server = json.load(stream)
assert server["id"] == sys.argv[2]
assert server["actual_state"] in {"disconnected", "connecting", "backoff"}
assert server["credential_configured"] is True
assert "credential_ref" not in server
PY

"$minitun_bin" --socket "$socket_path" tun add primary 22 6000 --name ssh \
    >"$runtime_dir/tunnel-add.out" 2>"$runtime_dir/tunnel-add.err"
if ! grep -q 'Status.*pending' "$runtime_dir/tunnel-add.out" ||
    [[ -s "$runtime_dir/tunnel-add.err" ]]; then
    printf 'offline tunnel add returned an invalid result\n' >&2
    exit 1
fi

"$minitun_bin" --socket "$socket_path" tun list primary --json \
    >"$runtime_dir/tunnel-list.json" 2>"$runtime_dir/tunnel-list.err"
tunnel_id=$(python3 - "$runtime_dir/tunnel-list.json" "$server_id" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    tunnels = json.load(stream)
assert len(tunnels) == 1
tunnel = tunnels[0]
assert tunnel["server_id"] == sys.argv[2]
assert tunnel["name"] == "ssh"
assert tunnel["local_endpoint"] == "127.0.0.1:22"
assert tunnel["remote_endpoint"] == "0.0.0.0:6000"
assert tunnel["desired_state"] == "active"
assert tunnel["actual_state"] == "pending"
print(tunnel["id"])
PY
)

"$minitun_bin" --socket "$socket_path" tun inspect "$tunnel_id" --json \
    >"$runtime_dir/tunnel-inspect.json" 2>"$runtime_dir/tunnel-inspect.err"
python3 - "$runtime_dir/tunnel-inspect.json" "$tunnel_id" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    tunnel = json.load(stream)
assert tunnel["id"] == sys.argv[2]
assert tunnel["actual_state"] == "pending"
PY

full_status=$("$minitun_bin" --socket "$socket_path" status)
if [[ "$full_status" != *"Servers  1 total, 0 online"* ]] ||
    [[ "$full_status" != *"Tunnels  1 total, 0 active"* ]]; then
    printf 'unexpected aggregate status output:\n%s\n' "$full_status" >&2
    exit 1
fi

python3 - "$credentials_path" <<'PY'
import os
import stat
import sys

mode = stat.S_IMODE(os.stat(sys.argv[1]).st_mode)
assert mode == 0o600, oct(mode)
PY

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

stop_daemon
start_daemon

"$minitun_bin" --socket "$socket_path" server inspect primary --json \
    >"$runtime_dir/server-recovered.json" 2>"$runtime_dir/server-recovered.err"
"$minitun_bin" --socket "$socket_path" tun inspect "$tunnel_id" --json \
    >"$runtime_dir/tunnel-recovered.json" 2>"$runtime_dir/tunnel-recovered.err"
python3 - "$runtime_dir/server-recovered.json" "$runtime_dir/tunnel-recovered.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    server = json.load(stream)
with open(sys.argv[2], encoding="utf-8") as stream:
    tunnel = json.load(stream)
assert server["actual_state"] in {"disconnected", "connecting", "backoff"}
assert server["credential_configured"] is True
assert tunnel["actual_state"] == "pending"
assert tunnel["desired_state"] == "active"
PY

"$minitun_bin" --socket "$socket_path" tun remove "$tunnel_id" \
    >"$runtime_dir/tunnel-remove.out" 2>"$runtime_dir/tunnel-remove.err"
set +e
"$minitun_bin" --socket "$socket_path" tun inspect "$tunnel_id" --json \
    >"$runtime_dir/tunnel-removed-inspect.out" 2>"$runtime_dir/tunnel-removed-inspect.err"
tunnel_removed_status=$?
set -e
if [[ $tunnel_removed_status -ne 2 ]]; then
    printf 'removed tunnel inspect returned %d, expected 2\n' "$tunnel_removed_status" >&2
    exit 1
fi

"$minitun_bin" --socket "$socket_path" server remove primary \
    >"$runtime_dir/server-remove.out" 2>"$runtime_dir/server-remove.err"
"$minitun_bin" --socket "$socket_path" server list --json \
    >"$runtime_dir/server-list-removed.json" 2>"$runtime_dir/server-list-removed.err"
"$minitun_bin" --socket "$socket_path" tun list --json \
    >"$runtime_dir/tunnel-list-removed.json" 2>"$runtime_dir/tunnel-list-removed.err"
python3 - "$runtime_dir/server-list-removed.json" "$runtime_dir/tunnel-list-removed.json" <<'PY'
import json
import sys

for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as stream:
        assert json.load(stream) == []
PY

python3 - "$state_path" "$credentials_path" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as state:
    assert state.execute("SELECT COUNT(*) FROM servers").fetchone()[0] == 0
    assert state.execute("SELECT COUNT(*) FROM tunnels").fetchone()[0] == 0
with sqlite3.connect(sys.argv[2]) as credentials:
    assert credentials.execute("SELECT COUNT(*) FROM credentials").fetchone()[0] == 0
PY

stop_daemon

python3 - "$state_path" "$credentials_path" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as state:
    assert state.execute("SELECT COUNT(*) FROM servers").fetchone()[0] == 0
    assert state.execute("SELECT COUNT(*) FROM tunnels").fetchone()[0] == 0
with sqlite3.connect(sys.argv[2]) as credentials:
    assert credentials.execute("SELECT COUNT(*) FROM credentials").fetchone()[0] == 0
PY

python3 - "$runtime_dir" "$orphan_token" "$stdin_token" "$interactive_token" <<'PY'
import os
import sys

root = sys.argv[1]
secrets = [value.encode() for value in sys.argv[2:]]
for directory, _, names in os.walk(root):
    for name in names:
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as stream:
            contents = stream.read()
        for secret in secrets:
            assert secret not in contents, path
PY

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
    ! grep -q 'internal failure.*protocol_error' "$runtime_dir/malformed.err"; then
    printf 'malformed daemon response exit code was %d, expected protocol failure 10\n' \
        "$malformed_status" >&2
    sed -n '1,120p' "$runtime_dir/malformed.out" >&2
    sed -n '1,120p' "$runtime_dir/malformed.err" >&2
    exit 1
fi

for error_spec in authentication_failed:4 connection_failed:5; do
    error_code=${error_spec%%:*}
    expected_status=${error_spec##*:}
    python3 - "$socket_path" "$error_code" \
        >"$runtime_dir/$error_code-peer.log" 2>&1 <<'PY' &
import json
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


socket_path, error_code = sys.argv[1:]
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.settimeout(5.0)
try:
    server.bind(socket_path)
    server.listen(1)
    connection, _ = server.accept()
    with connection:
        connection.settimeout(5.0)
        request_size = struct.unpack("!I", receive_exact(connection, 4))[0]
        request = json.loads(receive_exact(connection, request_size))
        response = json.dumps(
            {
                "version": 1,
                "request_id": request["request_id"],
                "ok": False,
                "error": {"code": error_code, "message": "simulated failure"},
            },
            separators=(",", ":"),
        ).encode()
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
            printf '%s IPC peer exited before creating its socket\n' "$error_code" >&2
            exit 1
        fi
        sleep 0.05
    done

    set +e
    "$minitun_bin" --socket "$socket_path" daemon status \
        >"$runtime_dir/$error_code.out" 2>"$runtime_dir/$error_code.err"
    actual_status=$?
    wait "$daemon_pid"
    peer_status=$?
    daemon_pid=
    set -e
    if [[ $peer_status -ne 0 || $actual_status -ne $expected_status ]]; then
        printf '%s response returned peer/client status %d/%d, expected 0/%d\n' \
            "$error_code" "$peer_status" "$actual_status" "$expected_status" >&2
        exit 1
    fi
done

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
    --database "$runtime_dir/invalid-state.db" \
    --credentials "$runtime_dir/invalid-credentials.db" \
    >"$runtime_dir/invalid-daemon.out" 2>"$runtime_dir/invalid-daemon.err"
invalid_daemon_status=$?
set -e
if [[ $invalid_client_status -ne 2 || $invalid_daemon_status -ne 2 ]]; then
    printf 'invalid socket path exit codes were client=%d daemon=%d, expected 2/2\n' \
        "$invalid_client_status" "$invalid_daemon_status" >&2
    exit 1
fi

printf 'CLI/daemon IPC integration passed\n'
