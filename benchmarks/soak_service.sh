#!/usr/bin/env bash
set -euo pipefail

action=${1:?usage: soak_service.sh start|status|collect PHASE SOURCE_SHA SESSION_ID ...}
phase=${2:?usage: soak_service.sh start|status|collect PHASE SOURCE_SHA SESSION_ID ...}
source_sha=${3:?usage: soak_service.sh start|status|collect PHASE SOURCE_SHA SESSION_ID ...}
session_id=${4:?usage: soak_service.sh start|status|collect PHASE SOURCE_SHA SESSION_ID ...}
script_dir=$(cd "$(dirname "$0")" && pwd -P)
soak_root=${MINITUN_SOAK_ROOT:-/var/lib/minitun-release-gates}

case "$phase" in
    full-24h) requested_seconds=86400 ;;
    mixed-7d) requested_seconds=604800 ;;
    *) printf 'invalid soak phase: %s\n' "$phase" >&2; exit 2 ;;
esac
if [[ ! "$source_sha" =~ ^[0-9a-f]{40}$ ]]; then
    printf 'source SHA must be a lowercase 40-character object ID\n' >&2
    exit 2
fi
if [[ ! "$session_id" =~ ^[0-9]+$ ]]; then
    printf 'session ID must be the numeric workflow run ID\n' >&2
    exit 2
fi

session_name="$phase-$source_sha-$session_id"
session_dir="$soak_root/$session_name"
unit_name="minitun-soak-${phase//[^a-zA-Z0-9]/-}-${source_sha:0:12}-$session_id"

verify_manifest() {
    python3 - "$session_dir/manifest.json" "$phase" "$source_sha" "$session_id" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

manifest_path = Path(sys.argv[1])
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
expected = {
    "phase": sys.argv[2],
    "source_commit": sys.argv[3],
    "session_id": sys.argv[4],
}
for field, value in expected.items():
    if str(manifest.get(field)) != value:
        raise SystemExit(f"manifest {field} mismatch")
root = manifest_path.parent
for name, expected_digest in manifest.get("sha256", {}).items():
    path = root / name
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != expected_digest:
        raise SystemExit(f"session file changed after launch: {name}")
PY
}

case "$action" in
    start)
        if ((EUID != 0)); then
            printf 'start must run as root (use sudo)\n' >&2
            exit 2
        fi
        minitun_bin=${5:?start requires minitun, minitund, and minitun-server paths}
        minitund_bin=${6:?start requires minitun, minitund, and minitun-server paths}
        server_bin=${7:?start requires minitun, minitund, and minitun-server paths}
        service_uid=${MINITUN_SOAK_UID:-${SUDO_UID:-}}
        service_gid=${MINITUN_SOAK_GID:-${SUDO_GID:-}}
        if [[ ! "$service_uid" =~ ^[1-9][0-9]*$ ||
                ! "$service_gid" =~ ^[1-9][0-9]*$ ]]; then
            printf 'start requires the non-root invoking UID/GID from sudo\n' >&2
            exit 2
        fi
        for executable in "$minitun_bin" "$minitund_bin" "$server_bin"; do
            [[ -x "$executable" ]] || {
                printf 'missing executable: %s\n' "$executable" >&2
                exit 2
            }
        done
        [[ ! -e "$session_dir" ]] || {
            printf 'soak session already exists: %s\n' "$session_dir" >&2
            exit 2
        }
        install -d -m 0755 "$soak_root" "$session_dir" "$session_dir/bin" \
            "$session_dir/tools" "$session_dir/results" "$session_dir/evidence"
        install -m 0755 "$minitun_bin" "$session_dir/bin/minitun"
        install -m 0755 "$minitund_bin" "$session_dir/bin/minitund"
        install -m 0755 "$server_bin" "$session_dir/bin/minitun-server"
        install -m 0755 "$script_dir/run_scale.sh" "$session_dir/tools/run_scale.sh"
        install -m 0755 "$script_dir/run_soak_gate.sh" \
            "$session_dir/tools/run_soak_gate.sh"
        install -m 0755 "$script_dir/run_persistent_soak.sh" \
            "$session_dir/tools/run_persistent_soak.sh"
        install -m 0755 "$script_dir/relay_load.py" "$session_dir/tools/relay_load.py"

        python3 - "$session_dir/manifest.json" "$phase" "$source_sha" "$session_id" \
            "$requested_seconds" "$session_dir" <<'PY'
import hashlib
import json
import os
import platform
import sys
from datetime import datetime, timezone
from pathlib import Path

destination = Path(sys.argv[1])
root = Path(sys.argv[6])
names = [
    "bin/minitun",
    "bin/minitund",
    "bin/minitun-server",
    "tools/run_scale.sh",
    "tools/run_soak_gate.sh",
    "tools/run_persistent_soak.sh",
    "tools/relay_load.py",
]
document = {
    "evidence_format": 1,
    "phase": sys.argv[2],
    "source_commit": sys.argv[3],
    "session_id": sys.argv[4],
    "requested_seconds": int(sys.argv[5]),
    "requested_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
    "hostname": platform.node(),
    "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
    "cpu_count": os.cpu_count(),
    "sha256": {
        name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in names
    },
}
temporary = destination.with_suffix(".tmp")
temporary.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                     encoding="utf-8")
os.chmod(temporary, 0o644)
os.replace(temporary, destination)
PY
        printf '%s\n' "$unit_name" >"$session_dir/unit.txt"
        chmod 0644 "$session_dir/unit.txt"
        chown -R "$service_uid:$service_gid" "$session_dir"
        systemd-run --quiet --unit="$unit_name" --service-type=exec \
            --uid="$service_uid" --gid="$service_gid" \
            --property="WorkingDirectory=$session_dir" \
            --property="StandardOutput=append:$session_dir/service.log" \
            --property="StandardError=append:$session_dir/service.log" \
            --property="LimitNOFILE=65536" \
            --property="NoNewPrivileges=true" \
            --property="PrivateDevices=true" \
            --property="PrivateTmp=true" \
            --property="ProtectHome=true" \
            --property="ProtectSystem=strict" \
            --property="ReadWritePaths=$session_dir" \
            --property="RestrictSUIDSGID=true" \
            /usr/bin/flock --nonblock /var/tmp/minitun-release-gate.lock \
            "$session_dir/tools/run_persistent_soak.sh" \
            "$session_dir" "$phase" "$source_sha" "$session_id"
        for _ in $(seq 1 100); do
            if [[ -f "$session_dir/status.json" ]] &&
                    python3 - "$session_dir/status.json" <<'PY'
import json
import sys
raise SystemExit(0 if json.load(open(sys.argv[1], encoding="utf-8"))["state"] == "running" else 1)
PY
            then
                printf 'started %s as systemd unit %s\n' "$session_name" "$unit_name"
                exit 0
            fi
            if systemctl is-failed --quiet "$unit_name"; then
                systemctl status --no-pager "$unit_name" >&2 || true
                exit 1
            fi
            sleep 0.1
        done
        printf 'persistent soak did not enter running state\n' >&2
        systemctl status --no-pager "$unit_name" >&2 || true
        exit 1
        ;;
    status)
        [[ -f "$session_dir/status.json" ]] || {
            printf 'unknown soak session: %s\n' "$session_name" >&2
            exit 2
        }
        cat "$session_dir/status.json"
        ;;
    collect)
        destination=${5:?collect requires a destination directory}
        [[ -f "$session_dir/status.json" && -f "$session_dir/manifest.json" ]] || {
            printf 'unknown or incomplete soak session: %s\n' "$session_name" >&2
            exit 2
        }
        verify_manifest
        python3 - "$session_dir/status.json" <<'PY'
import json
import sys
status = json.load(open(sys.argv[1], encoding="utf-8"))
if status.get("state") != "completed":
    raise SystemExit("soak is still running")
if status.get("exit_code") != 0:
    raise SystemExit(f"soak failed with exit code {status.get('exit_code')}")
PY
        result="$session_dir/results/$phase.json"
        [[ -f "$result" ]] || {
            printf 'successful soak is missing its result JSON\n' >&2
            exit 1
        }
        "$script_dir/verify_release_evidence.py" soak "$phase" "$result" \
            --source-sha "$source_sha" --status "$session_dir/status.json"
        [[ ! -e "$destination" ]] || {
            printf 'collection destination already exists: %s\n' "$destination" >&2
            exit 2
        }
        install -d -m 0755 "$destination"
        install -m 0644 "$result" "$destination/$phase.json"
        install -m 0644 "$session_dir/status.json" "$destination/status.json"
        install -m 0644 "$session_dir/manifest.json" "$destination/manifest.json"
        cp -R "$session_dir/evidence" "$destination/evidence"
        printf 'collected %s into %s\n' "$session_name" "$destination"
        ;;
    *)
        printf 'invalid action: %s\n' "$action" >&2
        exit 2
        ;;
esac
