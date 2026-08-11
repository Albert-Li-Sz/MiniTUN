#!/usr/bin/env bash
set -uo pipefail

session_dir=${1:?usage: run_persistent_soak.sh SESSION PHASE SOURCE_SHA SESSION_ID}
phase=${2:?usage: run_persistent_soak.sh SESSION PHASE SOURCE_SHA SESSION_ID}
source_sha=${3:?usage: run_persistent_soak.sh SESSION PHASE SOURCE_SHA SESSION_ID}
session_id=${4:?usage: run_persistent_soak.sh SESSION PHASE SOURCE_SHA SESSION_ID}
status_path="$session_dir/status.json"
started_epoch=$(date +%s)
started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
boot_id=$(cat /proc/sys/kernel/random/boot_id)

# A full-duration soak always uses the fixed scale and duration. The collector also
# validates the measured result, but clearing inherited development overrides
# prevents accidental under-testing on the dedicated host.
unset CLIENTS TUNNELS_PER_CLIENT CONNECTIONS_PER_TUNNEL BYTES_PER_CONNECTION
unset SOAK_SECONDS SOAK_SECONDS_OVERRIDE SOAK_BYTES_PER_CONNECTION SOAK_EVENTS
unset REMOTE_PORT_BASE RESULT_PATH EVIDENCE_DIR MINITUN_ALLOW_UNSUPPORTED_HOST

write_status() {
    local state=$1
    local exit_code=$2
    local finished_epoch=$3
    local finished_at=$4
    python3 - "$status_path" "$state" "$exit_code" "$started_epoch" "$started_at" \
        "$finished_epoch" "$finished_at" "$phase" "$source_sha" "$session_id" \
        "$boot_id" <<'PY'
import json
import os
import platform
import sys

destination = sys.argv[1]
document = {
    "evidence_format": 1,
    "state": sys.argv[2],
    "exit_code": int(sys.argv[3]),
    "started_epoch": int(sys.argv[4]),
    "started_at": sys.argv[5],
    "finished_epoch": int(sys.argv[6]),
    "finished_at": sys.argv[7],
    "phase": sys.argv[8],
    "source_commit": sys.argv[9],
    "session_id": sys.argv[10],
    "boot_id": sys.argv[11],
    "hostname": platform.node(),
}
temporary = destination + ".tmp"
with open(temporary, "w", encoding="utf-8") as stream:
    json.dump(document, stream, indent=2, sort_keys=True)
    stream.write("\n")
os.chmod(temporary, 0o644)
os.replace(temporary, destination)
PY
}

write_status running -1 0 ""
exit_code=0
RESULT_DIR="$session_dir/results" \
EVIDENCE_DIR="$session_dir/evidence" \
MINITUN_SOURCE_SHA="$source_sha" \
MINITUN_EVIDENCE_SESSION_ID="$session_id" \
MINITUN_KEEP_BENCHMARK_ARTIFACTS=0 \
    "$session_dir/tools/run_soak_gate.sh" "$phase" \
        "$session_dir/bin/minitun" \
        "$session_dir/bin/minitund" \
        "$session_dir/bin/minitun-server" || exit_code=$?

finished_epoch=$(date +%s)
finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
write_status completed "$exit_code" "$finished_epoch" "$finished_at"
exit "$exit_code"
