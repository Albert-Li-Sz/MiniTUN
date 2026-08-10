#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd -P)
runs=${RUNS:-3}
result_dir=${RESULT_DIR:-benchmark-results}
mkdir -p "$result_dir"

for run in $(seq 1 "$runs"); do
    if ! RESULT_PATH="$result_dir/run-$run.json" \
        EVIDENCE_DIR="$result_dir/run-$run-artifacts" \
        "$script_dir/run_scale.sh" "$@"; then
        printf 'benchmark run %s failed a gate; continuing to collect all runs\n' "$run" >&2
    fi
done

python3 - "$result_dir" "$runs" <<'PY'
import json
import statistics
import sys
from pathlib import Path

root = Path(sys.argv[1])
runs = [json.loads((root / f"run-{index}.json").read_text())
        for index in range(1, int(sys.argv[2]) + 1)]
throughputs = [run["minitun"]["payload_throughput_bits_per_second"] for run in runs]
ratios = [run["throughput_ratio"] for run in runs]
summary = {
    "evidence_format": 1,
    "source_commit": runs[0].get("source_commit", ""),
    "started_at": min(run["run_started_at"] for run in runs),
    "finished_at": max(run["soak_finished_at"] for run in runs),
    "independent_runs": len(runs),
    "median_payload_throughput_bits_per_second": statistics.median(throughputs),
    "median_throughput_ratio": statistics.median(ratios),
    "runs": runs,
}
failures = []
if any(run.get("source_commit", "") != summary["source_commit"] for run in runs):
    failures.append("source_commit_mismatch_between_runs")
if len(runs) < 3:
    failures.append("fewer_than_three_independent_runs")
if summary["median_payload_throughput_bits_per_second"] < 1_000_000_000:
    failures.append("median_throughput_below_1_gbit")
if summary["median_throughput_ratio"] < 0.85:
    failures.append("median_throughput_below_85_percent_of_baseline")
nonmedian_failures = {
    failure
    for run in runs
    for failure in run["failures"]
    if failure not in {
        "throughput_below_1_gbit",
        "throughput_below_85_percent_of_baseline",
    }
}
if nonmedian_failures:
    failures.append("one_or_more_runs_failed_a_nonmedian_gate")
summary["failures"] = failures
(root / "gate-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
print(json.dumps(summary, indent=2, sort_keys=True))
raise SystemExit(1 if failures else 0)
PY
