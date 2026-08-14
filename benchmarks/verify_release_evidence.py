#!/usr/bin/env python3
"""Validate MiniTUN benchmark, soak, and release-policy evidence."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path
from typing import Any

EVIDENCE_FORMAT = 1
EXPECTED_CLIENTS = 100
EXPECTED_TUNNELS = 2_000
EXPECTED_RELAYS = 10_000
MIN_THROUGHPUT = 1_000_000_000.0
MIN_BASELINE_RATIO = 0.85
MAX_P95_FIRST_BYTE_MS = 250.0
MAX_RSS_BYTES = 4 * 1024**3
MAX_RSS_DRIFT = 0.05
MAX_RESTART_CONVERGENCE_SECONDS = 30.0
MIN_MEMORY_BYTES = 7_800_000 * 1024
MAX_MEMORY_BYTES = 8_600_000 * 1024
PHASE_SECONDS = {"full-24h": 86_400, "mixed-7d": 604_800}
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
TAG_RE = re.compile(r"^v\d+\.\d+\.\d+(?:-rc\.(\d+))?$")


class EvidenceError(RuntimeError):
    """Raised when release evidence does not satisfy the frozen gate."""


def fail(message: str) -> None:
    raise EvidenceError(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read {path}: {error}")
    require(isinstance(value, dict), f"{path} must contain a JSON object")
    return value


def number(value: Any, field: str) -> float:
    require(isinstance(value, (int, float)) and not isinstance(value, bool),
            f"{field} must be numeric")
    return float(value)


def integer(value: Any, field: str) -> int:
    require(isinstance(value, int) and not isinstance(value, bool),
            f"{field} must be an integer")
    return value


def timestamp(value: Any, field: str) -> dt.datetime:
    require(isinstance(value, str) and value, f"{field} must be a timestamp")
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = dt.datetime.fromisoformat(normalized)
    except ValueError as error:
        fail(f"{field} is not ISO-8601: {error}")
    require(parsed.tzinfo is not None, f"{field} must include a timezone")
    return parsed.astimezone(dt.timezone.utc)


def validate_source(document: dict[str, Any], source_sha: str, context: str) -> None:
    require(document.get("evidence_format") == EVIDENCE_FORMAT,
            f"{context}: unsupported evidence_format")
    require(document.get("source_commit") == source_sha,
            f"{context}: source_commit does not match the release commit")


def validate_environment(document: dict[str, Any], context: str) -> None:
    environment = document.get("environment")
    require(isinstance(environment, dict), f"{context}: environment is missing")
    require(environment.get("system") == "Linux", f"{context}: host must be Linux")
    require(integer(environment.get("cpu_count"), f"{context}.environment.cpu_count") == 4,
            f"{context}: host must have exactly 4 CPUs")
    memory = integer(environment.get("memory_bytes"),
                     f"{context}.environment.memory_bytes")
    require(MIN_MEMORY_BYTES <= memory <= MAX_MEMORY_BYTES,
            f"{context}: host memory is outside the 8 GiB release range")


def validate_scale(document: dict[str, Any], context: str) -> None:
    scale = document.get("scale")
    require(isinstance(scale, dict), f"{context}: scale is missing")
    require(scale.get("clients") == EXPECTED_CLIENTS,
            f"{context}: expected {EXPECTED_CLIENTS} clients")
    require(scale.get("tunnels") == EXPECTED_TUNNELS,
            f"{context}: expected {EXPECTED_TUNNELS} tunnels")
    require(scale.get("concurrent_relays") == EXPECTED_RELAYS,
            f"{context}: expected {EXPECTED_RELAYS} concurrent relays")


def validate_common_run(document: dict[str, Any], source_sha: str, context: str,
                        *, allow_median_throughput_failures: bool) -> None:
    validate_source(document, source_sha, context)
    validate_environment(document, context)
    validate_scale(document, context)
    failures = document.get("failures")
    require(isinstance(failures, list) and all(isinstance(item, str) for item in failures),
            f"{context}: failures must be a string array")
    allowed = {
        "throughput_below_1_gbit",
        "throughput_below_85_percent_of_baseline",
    } if allow_median_throughput_failures else set()
    unexpected = sorted(set(failures) - allowed)
    require(not unexpected, f"{context}: failed gates: {', '.join(unexpected)}")

    tunnel = document.get("minitun")
    require(isinstance(tunnel, dict), f"{context}: minitun measurement is missing")
    require(integer(tunnel.get("connections_attempted"),
                    f"{context}.minitun.connections_attempted") == EXPECTED_RELAYS,
            f"{context}: not all relay connections were attempted")
    require(integer(tunnel.get("connections_succeeded"),
                    f"{context}.minitun.connections_succeeded") == EXPECTED_RELAYS,
            f"{context}: not all relay connections succeeded")
    require(tunnel.get("connections_failed") == 0,
            f"{context}: relay connections failed")
    require(tunnel.get("data_corruption") == 0, f"{context}: data corruption detected")
    latency = tunnel.get("first_byte_latency_ms")
    require(isinstance(latency, dict), f"{context}: latency measurement is missing")
    require(number(latency.get("p95"), f"{context}.latency.p95")
            <= MAX_P95_FIRST_BYTE_MS, f"{context}: p95 first-byte latency exceeded 250 ms")
    require(number(document.get("peak_minitun_rss_bytes"), f"{context}.peak_rss")
            <= MAX_RSS_BYTES, f"{context}: peak RSS exceeded 4 GiB")
    require(number(document.get("restart_convergence_seconds"),
                   f"{context}.restart_convergence_seconds")
            <= MAX_RESTART_CONVERGENCE_SECONDS,
            f"{context}: restart convergence exceeded 30 seconds")


def validate_performance(path: Path, source_sha: str) -> dict[str, Any]:
    summary = load_json(path)
    validate_source(summary, source_sha, "performance")
    require(summary.get("failures") == [], "performance: aggregate gate failed")
    run_count = integer(summary.get("independent_runs"), "performance.independent_runs")
    require(run_count >= 3, "performance: fewer than three independent runs")
    require(number(summary.get("median_payload_throughput_bits_per_second"),
                   "performance.median_throughput") >= MIN_THROUGHPUT,
            "performance: median throughput is below 1 Gbit/s")
    require(number(summary.get("median_throughput_ratio"), "performance.median_ratio")
            >= MIN_BASELINE_RATIO,
            "performance: median throughput is below 85% of the TLS/TCP baseline")
    runs = summary.get("runs")
    require(isinstance(runs, list) and len(runs) == run_count,
            "performance: run array does not match independent_runs")
    for index, run in enumerate(runs, 1):
        require(isinstance(run, dict), f"performance.run-{index}: must be an object")
        validate_common_run(run, source_sha, f"performance.run-{index}",
                            allow_median_throughput_failures=True)
        require(run.get("soak_seconds") == 0,
                f"performance.run-{index}: benchmark run unexpectedly contains a soak")
    started = timestamp(summary.get("started_at"), "performance.started_at")
    finished = timestamp(summary.get("finished_at"), "performance.finished_at")
    require(finished >= started, "performance: timestamps are reversed")
    return summary


def validate_soak(path: Path, source_sha: str, phase: str,
                  status_path: Path | None = None) -> dict[str, Any]:
    required_seconds = PHASE_SECONDS[phase]
    summary = load_json(path)
    validate_common_run(summary, source_sha, phase,
                        allow_median_throughput_failures=False)
    require(summary.get("soak_seconds") == required_seconds,
            f"{phase}: requested duration is not the release duration")
    elapsed = integer(summary.get("soak_elapsed_seconds"), f"{phase}.soak_elapsed_seconds")
    require(elapsed >= required_seconds, f"{phase}: measured soak duration is too short")
    require(integer(summary.get("soak_cycles"), f"{phase}.soak_cycles") > 0,
            f"{phase}: no full-scale load cycle completed")
    expected_events = phase == "mixed-7d"
    require(summary.get("soak_events_enabled") is expected_events,
            f"{phase}: disruption event mode is incorrect")
    require(number(summary.get("minitun", {}).get("payload_throughput_bits_per_second"),
                   f"{phase}.throughput") >= MIN_THROUGHPUT,
            f"{phase}: throughput is below 1 Gbit/s")
    require(number(summary.get("throughput_ratio"), f"{phase}.throughput_ratio")
            >= MIN_BASELINE_RATIO,
            f"{phase}: throughput is below 85% of the TLS/TCP baseline")
    require(number(summary.get("stable_rss_drift_fraction"), f"{phase}.rss_drift")
            <= MAX_RSS_DRIFT, f"{phase}: stable RSS drift exceeded 5%")
    started = timestamp(summary.get("soak_started_at"), f"{phase}.soak_started_at")
    finished = timestamp(summary.get("soak_finished_at"), f"{phase}.soak_finished_at")
    wall_seconds = (finished - started).total_seconds()
    require(wall_seconds >= required_seconds,
            f"{phase}: wall-clock timestamps prove a shorter soak")
    require(abs(wall_seconds - elapsed) <= 120,
            f"{phase}: monotonic and wall-clock durations disagree by more than 120 seconds")

    if status_path is not None:
        status = load_json(status_path)
        require(status.get("state") == "completed" and status.get("exit_code") == 0,
                f"{phase}: persistent service did not complete successfully")
        require(status.get("phase") == phase, f"{phase}: service phase mismatch")
        require(status.get("source_commit") == source_sha,
                f"{phase}: service source commit mismatch")
        require(status.get("session_id") == summary.get("evidence_session_id"),
                f"{phase}: service session mismatch")
        service_started = timestamp(status.get("started_at"), f"{phase}.status.started_at")
        service_finished = timestamp(status.get("finished_at"), f"{phase}.status.finished_at")
        require(service_started <= started and service_finished >= finished,
                f"{phase}: result timestamps fall outside the service lifetime")
    return summary


def validate_release(args: argparse.Namespace) -> dict[str, Any]:
    match = TAG_RE.fullmatch(args.tag)
    require(match is not None, "release: invalid tag")
    rc_number = int(match.group(1)) if match.group(1) else None
    result: dict[str, Any] = {
        "tag": args.tag,
        "source_commit": args.source_sha,
        "performance": "not-required",
        "full_24h": "not-required",
        "mixed_7d": "not-required",
    }
    if rc_number is not None:
        result["release_kind"] = "release-candidate"
        result["rc_number"] = rc_number
        return result

    result["release_kind"] = "general-availability"
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)

    performance = subcommands.add_parser("performance")
    performance.add_argument("evidence", type=Path)
    performance.add_argument("--source-sha", required=True)

    soak = subcommands.add_parser("soak")
    soak.add_argument("phase", choices=sorted(PHASE_SECONDS))
    soak.add_argument("evidence", type=Path)
    soak.add_argument("--source-sha", required=True)
    soak.add_argument("--status", type=Path)

    release = subcommands.add_parser("release")
    release.add_argument("--tag", required=True)
    release.add_argument("--source-sha", required=True)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    require(SHA_RE.fullmatch(args.source_sha) is not None,
            "source SHA must be a lowercase 40-character Git object ID")
    if args.command == "performance":
        validate_performance(args.evidence, args.source_sha)
        result = {"performance": "passed", "source_commit": args.source_sha}
    elif args.command == "soak":
        validate_soak(args.evidence, args.source_sha, args.phase, args.status)
        result = {args.phase: "passed", "source_commit": args.source_sha}
    else:
        result = validate_release(args)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except EvidenceError as error:
        print(f"release evidence rejected: {error}", file=sys.stderr)
        raise SystemExit(1) from None
