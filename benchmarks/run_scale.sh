#!/usr/bin/env bash
set -euo pipefail

minitun_bin=${MINITUN_BIN:-${1:-build/release/minitun}}
minitund_bin=${MINITUND_BIN:-${2:-build/release/minitund}}
server_bin=${MINITUN_SERVER_BIN:-${3:-build/release/minitun-server}}
script_dir=$(cd "$(dirname "$0")" && pwd -P)
load_tool="$script_dir/relay_load.py"

clients=${CLIENTS:-100}
tunnels_per_client=${TUNNELS_PER_CLIENT:-20}
connections_per_tunnel=${CONNECTIONS_PER_TUNNEL:-5}
bytes_per_connection=${BYTES_PER_CONNECTION:-1048576}
remote_port_base=${REMOTE_PORT_BASE:-12000}
soak_seconds=${SOAK_SECONDS:-0}
soak_bytes_per_connection=${SOAK_BYTES_PER_CONNECTION:-262144}
soak_events=${SOAK_EVENTS:-1}
result_path=${RESULT_PATH:-}
evidence_dir=${EVIDENCE_DIR:-}
source_commit=${MINITUN_SOURCE_SHA:-}
evidence_session_id=${MINITUN_EVIDENCE_SESSION_ID:-}
total_tunnels=$((clients * tunnels_per_client))
total_connections=$((total_tunnels * connections_per_tunnel))
run_started_epoch=$(date +%s)
run_started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
if [[ -n "$evidence_dir" ]]; then
    install -d -m 0755 "$evidence_dir"
    evidence_dir=$(cd "$evidence_dir" && pwd -P)
fi

if ((total_tunnels != 2000 || total_connections != 10000)); then
    printf 'warning: non-release benchmark scale: %s clients, %s tunnels, %s connections\n' \
        "$clients" "$total_tunnels" "$total_connections" >&2
fi
if ((remote_port_base < 1024 || remote_port_base + total_tunnels - 1 > 65535)); then
    printf 'remote benchmark port range is invalid\n' >&2
    exit 2
fi
for executable in "$minitun_bin" "$minitund_bin" "$server_bin" "$load_tool"; do
    if [[ ! -x "$executable" ]]; then
        printf 'missing benchmark executable: %s\n' "$executable" >&2
        exit 2
    fi
done
command -v curl >/dev/null
command -v openssl >/dev/null
command -v python3 >/dev/null
if [[ $(uname -s) != Linux && ${MINITUN_ALLOW_UNSUPPORTED_HOST:-0} != 1 ]]; then
    printf 'the scale gate requires the dedicated Linux benchmark host\n' >&2
    exit 2
fi
ulimit -n 65536

runtime_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
runtime_dir=$(mktemp -d "$runtime_root/minitun-scale.XXXXXX")
daemon_pids=()
daemon_sockets=()
server_pid=
backend_pid=
baseline_pid=
rss_sampler_pid=

# shellcheck disable=SC2329  # Invoked by the EXIT trap below.
# shellcheck disable=SC2317  # Invoked by the EXIT trap below.
cleanup() {
    : >"$runtime_dir/stop-rss-sampler" 2>/dev/null || true
    for process_id in "$rss_sampler_pid" "$baseline_pid" "$backend_pid" "$server_pid" \
            "${daemon_pids[@]:-}"; do
        if [[ -n "$process_id" ]] && kill -0 "$process_id" 2>/dev/null; then
            kill -TERM "$process_id" 2>/dev/null || true
            wait "$process_id" 2>/dev/null || true
        fi
    done
    if [[ ${MINITUN_KEEP_BENCHMARK_ARTIFACTS:-0} == 1 ]]; then
        printf 'kept benchmark artifacts at %s\n' "$runtime_dir" >&2
    else
        rm -rf "$runtime_dir"
    fi
}
trap cleanup EXIT

select_port() {
    python3 - <<'PY'
import socket

probe = socket.socket()
probe.bind(("127.0.0.1", 0))
print(probe.getsockname()[1])
probe.close()
PY
}

openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost \
    -keyout "$runtime_dir/server.key" \
    -out "$runtime_dir/server.crt" >/dev/null 2>&1
chmod 0600 "$runtime_dir/server.key"

baseline_control_port=$(select_port)
printf '%s\n' "$baseline_control_port" >"$runtime_dir/baseline-ports"
"$load_tool" echo --host 127.0.0.1 --port "$baseline_control_port" --tls \
    --cert "$runtime_dir/server.crt" --key "$runtime_dir/server.key" \
    >"$runtime_dir/baseline-listener.log" 2>&1 &
baseline_pid=$!
for _ in $(seq 1 100); do
    if python3 - "$baseline_control_port" 2>/dev/null <<'PY'
import socket
import sys
connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.1)
connection.close()
PY
    then
        break
    fi
    sleep 0.05
done
"$load_tool" load \
    --host localhost \
    --ports-file "$runtime_dir/baseline-ports" \
    --connections-per-port "$total_connections" \
    --bytes-per-connection "$bytes_per_connection" \
    --tls --ca "$runtime_dir/server.crt" \
    --result "$runtime_dir/baseline.json" >"$runtime_dir/baseline.stdout"
kill -TERM "$baseline_pid"
wait "$baseline_pid"
baseline_pid=

"$load_tool" echo --host 127.0.0.1 --port 0 >"$runtime_dir/backend-port" 2>&1 &
backend_pid=$!
for _ in $(seq 1 100); do
    [[ -s "$runtime_dir/backend-port" ]] && break
    sleep 0.05
done
backend_port=$(head -n 1 "$runtime_dir/backend-port")
[[ "$backend_port" =~ ^[0-9]+$ ]]

mkdir -p "$runtime_dir/clients"
: >"$runtime_dir/client-map"
for index in $(seq 0 $((clients - 1))); do
    client_dir="$runtime_dir/clients/$index"
    mkdir -p "$client_dir"
    socket_path="$client_dir/minitun.sock"
    openssl rand -hex 32 >"$client_dir/psk"
    chmod 0600 "$client_dir/psk"
    "$minitund_bin" --foreground \
        --socket "$socket_path" \
        --database "$client_dir/state.db" \
        --credentials "$client_dir/credentials.db" \
        --max-idle-workers-per-server 128 \
        --max-total-idle-workers 128 \
        --max-total-connections 128 \
        --io-threads 1 \
        --log-level warn >"$client_dir/minitund.log" 2>&1 &
    daemon_pids+=("$!")
    daemon_sockets+=("$socket_path")
done

for index in $(seq 0 $((clients - 1))); do
    socket_path=${daemon_sockets[$index]}
    process_id=${daemon_pids[$index]}
    for _ in $(seq 1 200); do
        [[ -S "$socket_path" ]] && break
        if ! kill -0 "$process_id" 2>/dev/null; then
            cat "$runtime_dir/clients/$index/minitund.log" >&2
            exit 1
        fi
        sleep 0.05
    done
    client_id=$(
        "$minitun_bin" --socket "$socket_path" daemon identity --json |
            python3 -c 'import json,sys; print(json.load(sys.stdin)["client_id"])'
    )
    printf '%s\t%s\n' "$client_id" "$runtime_dir/clients/$index/psk" \
        >>"$runtime_dir/client-map"
done

python3 - "$runtime_dir/client-map" "$runtime_dir/clients.json" \
    "$remote_port_base" "$((remote_port_base + total_tunnels - 1))" \
    "$tunnels_per_client" "$connections_per_tunnel" <<'PY'
import json
import os
import sys

mapping, destination = sys.argv[1:3]
allowed = f"{sys.argv[3]}-{sys.argv[4]}"
max_tunnels = int(sys.argv[5])
connections_per_tunnel = int(sys.argv[6])
clients = []
with open(mapping, encoding="utf-8") as stream:
    for line in stream:
        client_id, psk_path = line.rstrip("\n").split("\t")
        clients.append({
            "client_id": client_id,
            "enabled": True,
            "psk_file": psk_path,
            "allowed_ports": [allowed],
            "max_tunnels": max_tunnels,
            "max_connections": max_tunnels * connections_per_tunnel,
            "max_idle_workers": 128,
        })
document = {"format_version": 1, "clients": clients}
temporary = destination + ".tmp"
with open(temporary, "w", encoding="utf-8") as stream:
    json.dump(document, stream, separators=(",", ":"), sort_keys=True)
    stream.write("\n")
os.chmod(temporary, 0o600)
os.replace(temporary, destination)
PY

control_port=$(select_port)
admin_port=$(select_port)
start_server() {
    "$server_bin" --foreground \
        --listen "127.0.0.1:$control_port" \
        --tls-cert "$runtime_dir/server.crt" \
        --tls-key "$runtime_dir/server.key" \
        --clients-config "$runtime_dir/clients.json" \
        --admin-listen "127.0.0.1:$admin_port" \
        --max-clients "$clients" \
        --max-tunnels-per-client "$tunnels_per_client" \
        --max-total-tunnels "$total_tunnels" \
        --max-connections-per-client "$((tunnels_per_client * connections_per_tunnel))" \
        --max-total-connections "$total_connections" \
        --min-idle-workers 32 \
        --max-idle-workers 128 \
        --max-total-idle-workers 4096 \
        --worker-wait-timeout 5 \
        --io-threads 4 \
        --log-level warn >>"$runtime_dir/server.log" 2>&1 &
    server_pid=$!
    for _ in $(seq 1 200); do
        if curl --fail --silent "http://127.0.0.1:$admin_port/readyz" >/dev/null; then
            return
        fi
        if ! kill -0 "$server_pid" 2>/dev/null; then
            cat "$runtime_dir/server.log" >&2
            exit 1
        fi
        sleep 0.05
    done
    printf 'server did not become ready\n' >&2
    exit 1
}
start_server

python3 - "$runtime_dir" "$clients" "$tunnels_per_client" "$backend_port" \
    "$control_port" "$remote_port_base" <<'PY'
import json
import os
import sys

root = sys.argv[1]
clients = int(sys.argv[2])
tunnels_per_client = int(sys.argv[3])
backend_port = int(sys.argv[4])
control_port = int(sys.argv[5])
base = int(sys.argv[6])
for client in range(clients):
    client_dir = os.path.join(root, "clients", str(client))
    tunnels = []
    for offset in range(tunnels_per_client):
        global_index = client * tunnels_per_client + offset
        tunnels.append({
            "name": f"tunnel-{offset}",
            "server": "edge",
            "local_host": "127.0.0.1",
            "local_port": backend_port,
            "remote_port": base + global_index,
            "enabled": True,
        })
    document = {
        "format_version": 1,
        "servers": [{
            "name": "edge",
            "endpoint": f"localhost:{control_port}",
            "tls_server_name": "localhost",
            "psk_file": "psk",
            "ca_file": os.path.join(root, "server.crt"),
            "enabled": True,
        }],
        "tunnels": tunnels,
    }
    path = os.path.join(client_dir, "config.json")
    with open(path, "w", encoding="utf-8") as stream:
        json.dump(document, stream, separators=(",", ":"), sort_keys=True)
        stream.write("\n")
    os.chmod(path, 0o600)
PY

apply_pids=()
for index in $(seq 0 $((clients - 1))); do
    "$minitun_bin" --socket "${daemon_sockets[$index]}" \
        config apply "$runtime_dir/clients/$index/config.json" \
        >"$runtime_dir/clients/$index/apply.log" 2>&1 &
    apply_pids+=("$!")
done
for process_id in "${apply_pids[@]}"; do
    wait "$process_id"
done

metrics_tunnels() {
    curl --fail --silent "http://127.0.0.1:$admin_port/metrics" |
        awk '$1 == "minitun_tunnels" {print int($2); exit}'
}
wait_tunnels() {
    local deadline=$((SECONDS + 60))
    while ((SECONDS < deadline)); do
        if [[ $(metrics_tunnels 2>/dev/null || true) == "$total_tunnels" ]]; then
            return
        fi
        sleep 0.1
    done
    curl --silent "http://127.0.0.1:$admin_port/metrics" >&2 || true
    printf 'tunnels did not converge to %s\n' "$total_tunnels" >&2
    exit 1
}
wait_tunnels

kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
restart_started_ns=$(python3 -c 'import time; print(time.monotonic_ns())')
start_server
wait_tunnels
restart_finished_ns=$(python3 -c 'import time; print(time.monotonic_ns())')
convergence_seconds=$(python3 - "$restart_started_ns" "$restart_finished_ns" <<'PY'
import sys
print((int(sys.argv[2]) - int(sys.argv[1])) / 1_000_000_000)
PY
)

seq "$remote_port_base" "$((remote_port_base + total_tunnels - 1))" \
    >"$runtime_dir/tunnel-ports"

current_rss_kib() {
    local total=0
    local process_id value
    for process_id in "$server_pid" "${daemon_pids[@]}"; do
        if [[ -r "/proc/$process_id/status" ]]; then
            value=$(awk '$1 == "VmRSS:" {print $2; exit}' "/proc/$process_id/status")
            total=$((total + ${value:-0}))
        fi
    done
    printf '%s\n' "$total"
}

sample_rss() {
    local peak=0 current
    while [[ ! -e "$runtime_dir/stop-rss-sampler" ]]; do
        current=$(current_rss_kib)
        ((current > peak)) && peak=$current
        printf '%s\n' "$peak" >"$runtime_dir/peak-rss-kib"
        sleep 0.2
    done
}
sample_rss &
rss_sampler_pid=$!

"$load_tool" load \
    --host 127.0.0.1 \
    --ports-file "$runtime_dir/tunnel-ports" \
    --connections-per-port "$connections_per_tunnel" \
    --bytes-per-connection "$bytes_per_connection" \
    --result "$runtime_dir/minitun.json" >"$runtime_dir/minitun.stdout"
warm_rss_kib=$(current_rss_kib)

soak_started_epoch=$(date +%s)
soak_started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
soak_started_seconds=$SECONDS
cycle=0
printf 'soak_started phase=%s requested_seconds=%s at=%s\n' \
    "$([[ $soak_events == 0 ]] && printf full || printf mixed)" \
    "$soak_seconds" "$soak_started_at" >"$runtime_dir/events.log"
while ((soak_seconds > 0 && SECONDS - soak_started_seconds < soak_seconds)); do
    cycle=$((cycle + 1))
    "$load_tool" load \
        --host 127.0.0.1 \
        --ports-file "$runtime_dir/tunnel-ports" \
        --connections-per-port "$connections_per_tunnel" \
        --bytes-per-connection "$soak_bytes_per_connection" \
        --result "$runtime_dir/soak-$cycle.json" >"$runtime_dir/soak-$cycle.stdout"
    if ((soak_events != 0 && cycle % 3 == 0)); then
        printf 'policy_reload cycle=%s at=%s\n' "$cycle" \
            "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$runtime_dir/events.log"
        kill -HUP "$server_pid"
        wait_tunnels
    fi
    if ((soak_events != 0 && cycle % 6 == 0)); then
        printf 'server_restart_and_network_window cycle=%s at=%s\n' "$cycle" \
            "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$runtime_dir/events.log"
        kill -TERM "$server_pid"
        wait "$server_pid"
        server_pid=
        sleep 2
        start_server
        wait_tunnels
    fi
    if ((soak_events != 0 && cycle % 9 == 0)); then
        rotation_index=$((cycle / 9 % clients))
        printf 'psk_rotation cycle=%s client_index=%s at=%s\n' \
            "$cycle" "$rotation_index" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            >>"$runtime_dir/events.log"
        openssl rand -hex 32 >"$runtime_dir/clients/$rotation_index/psk.new"
        chmod 0600 "$runtime_dir/clients/$rotation_index/psk.new"
        mv "$runtime_dir/clients/$rotation_index/psk.new" \
            "$runtime_dir/clients/$rotation_index/psk"
        kill -HUP "$server_pid"
        "$minitun_bin" --socket "${daemon_sockets[$rotation_index]}" \
            config apply "$runtime_dir/clients/$rotation_index/config.json" >/dev/null
        wait_tunnels
    fi
done
soak_finished_epoch=$(date +%s)
soak_finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
soak_elapsed_seconds=$((SECONDS - soak_started_seconds))
printf 'soak_finished cycles=%s elapsed_seconds=%s at=%s\n' \
    "$cycle" "$soak_elapsed_seconds" "$soak_finished_at" \
    >>"$runtime_dir/events.log"

final_rss_kib=$(current_rss_kib)
: >"$runtime_dir/stop-rss-sampler"
wait "$rss_sampler_pid"
rss_sampler_pid=
peak_rss_kib=$(cat "$runtime_dir/peak-rss-kib")

set +e
python3 - "$runtime_dir/baseline.json" "$runtime_dir/minitun.json" \
    "$convergence_seconds" "$peak_rss_kib" "$warm_rss_kib" "$final_rss_kib" \
    "$clients" "$total_tunnels" "$total_connections" "$soak_seconds" \
    "$runtime_dir/summary.json" "$source_commit" "$evidence_session_id" \
    "$run_started_epoch" "$run_started_at" "$soak_started_epoch" \
    "$soak_started_at" "$soak_finished_epoch" "$soak_finished_at" \
    "$soak_elapsed_seconds" "$cycle" "$soak_events" <<'PY'
import json
import os
import platform
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    baseline = json.load(stream)
with open(sys.argv[2], encoding="utf-8") as stream:
    tunnel = json.load(stream)
convergence = float(sys.argv[3])
peak_rss = int(sys.argv[4]) * 1024
warm_rss = int(sys.argv[5]) * 1024
final_rss = int(sys.argv[6]) * 1024
ratio = (tunnel["payload_throughput_bits_per_second"] /
         baseline["payload_throughput_bits_per_second"])
drift = ((final_rss - warm_rss) / warm_rss) if warm_rss else 0.0
memory_bytes = 0
try:
    with open("/proc/meminfo", encoding="ascii") as stream:
        for line in stream:
            if line.startswith("MemTotal:"):
                memory_bytes = int(line.split()[1]) * 1024
                break
except OSError:
    pass
summary = {
    "evidence_format": 1,
    "source_commit": sys.argv[12],
    "evidence_session_id": sys.argv[13],
    "run_started_epoch": int(sys.argv[14]),
    "run_started_at": sys.argv[15],
    "soak_started_epoch": int(sys.argv[16]),
    "soak_started_at": sys.argv[17],
    "soak_finished_epoch": int(sys.argv[18]),
    "soak_finished_at": sys.argv[19],
    "soak_elapsed_seconds": int(sys.argv[20]),
    "soak_cycles": int(sys.argv[21]),
    "soak_events_enabled": bool(int(sys.argv[22])),
    "environment": {
        "platform": platform.platform(),
        "system": platform.system(),
        "machine": platform.machine(),
        "hostname": platform.node(),
        "cpu_count": os.cpu_count(),
        "memory_bytes": memory_bytes,
        "runner_image_digest": os.environ.get("MINITUN_RUNNER_IMAGE_DIGEST", ""),
    },
    "scale": {
        "clients": int(sys.argv[7]),
        "tunnels": int(sys.argv[8]),
        "concurrent_relays": int(sys.argv[9]),
    },
    "baseline": baseline,
    "minitun": tunnel,
    "throughput_ratio": ratio,
    "restart_convergence_seconds": convergence,
    "peak_minitun_rss_bytes": peak_rss,
    "stable_rss_drift_fraction": drift,
    "soak_seconds": int(sys.argv[10]),
}
failures = []
if tunnel["payload_throughput_bits_per_second"] < 1_000_000_000:
    failures.append("throughput_below_1_gbit")
if ratio < 0.85:
    failures.append("throughput_below_85_percent_of_baseline")
if tunnel["first_byte_latency_ms"]["p95"] > 250:
    failures.append("p95_first_byte_above_250_ms")
if tunnel["connections_failed"] or tunnel["data_corruption"]:
    failures.append("relay_failure_or_corruption")
if peak_rss > 4 * 1024**3:
    failures.append("rss_above_4_gib")
if convergence > 30:
    failures.append("restart_convergence_above_30_seconds")
if int(sys.argv[10]) and drift > 0.05:
    failures.append("stable_rss_drift_above_5_percent")
summary["failures"] = failures
with open(sys.argv[11], "w", encoding="utf-8") as stream:
    json.dump(summary, stream, indent=2, sort_keys=True)
    stream.write("\n")
print(json.dumps(summary, indent=2, sort_keys=True))
raise SystemExit(1 if failures else 0)
PY
gate_status=$?
set -e

if [[ -n "$result_path" ]]; then
    install -m 0644 "$runtime_dir/summary.json" "$result_path"
fi
if [[ -n "$evidence_dir" ]]; then
    install -m 0644 "$runtime_dir/summary.json" "$evidence_dir/summary.json"
    install -m 0644 "$runtime_dir/baseline.json" "$evidence_dir/baseline.json"
    install -m 0644 "$runtime_dir/minitun.json" "$evidence_dir/minitun.json"
    install -m 0644 "$runtime_dir/events.log" "$evidence_dir/events.log"
    install -m 0644 "$runtime_dir/server.log" "$evidence_dir/server.log"
    (
        cd "$runtime_dir/clients"
        find . -type f \( -name minitund.log -o -name apply.log \) -print0 |
            tar --null -czf "$evidence_dir/daemon-logs.tar.gz" --files-from=-
    )
    uname -a >"$evidence_dir/uname.txt"
    if [[ -r /proc/cpuinfo ]]; then
        install -m 0644 /proc/cpuinfo "$evidence_dir/cpuinfo.txt"
    fi
    if [[ -r /proc/meminfo ]]; then
        install -m 0644 /proc/meminfo "$evidence_dir/meminfo.txt"
    fi
fi
exit "$gate_status"
