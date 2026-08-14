#!/usr/bin/env bash
set -euo pipefail

phase=${1:?usage: run_soak_gate.sh full-24h|mixed-7d [minitun minitund minitun-server]}
shift
result_dir=${RESULT_DIR:-soak-results}
mkdir -p "$result_dir"

case "$phase" in
    full-24h)
        duration=${SOAK_SECONDS_OVERRIDE:-86400}
        events=0
        bytes=${SOAK_BYTES_PER_CONNECTION:-1048576}
        ;;
    mixed-7d)
        duration=${SOAK_SECONDS_OVERRIDE:-604800}
        events=1
        bytes=${SOAK_BYTES_PER_CONNECTION:-262144}
        ;;
    *)
        printf 'unknown soak phase: %s\n' "$phase" >&2
        exit 2
        ;;
esac

SOAK_SECONDS="$duration" \
SOAK_EVENTS="$events" \
SOAK_BYTES_PER_CONNECTION="$bytes" \
RESULT_PATH="$result_dir/$phase.json" \
    "$(cd "$(dirname "$0")" && pwd -P)/run_scale.sh" "$@"
