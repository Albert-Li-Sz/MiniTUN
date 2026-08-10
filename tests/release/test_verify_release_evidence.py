#!/usr/bin/env python3

from __future__ import annotations

import copy
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path

SOURCE_ROOT = Path(os.environ["MINITUN_SOURCE_DIR"])
SPEC = importlib.util.spec_from_file_location(
    "verify_release_evidence",
    SOURCE_ROOT / "benchmarks" / "verify_release_evidence.py",
)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)

SHA = "a" * 40


def base_run() -> dict:
    return {
        "evidence_format": 1,
        "source_commit": SHA,
        "evidence_session_id": "123",
        "run_started_epoch": 1_700_000_000,
        "run_started_at": "2026-08-01T00:00:00Z",
        "soak_started_epoch": 1_700_000_100,
        "soak_started_at": "2026-08-01T00:01:40Z",
        "soak_finished_epoch": 1_700_000_100,
        "soak_finished_at": "2026-08-01T00:01:40Z",
        "soak_elapsed_seconds": 0,
        "soak_cycles": 0,
        "soak_events_enabled": False,
        "environment": {
            "platform": "Linux-test",
            "system": "Linux",
            "machine": "x86_64",
            "hostname": "benchmark",
            "cpu_count": 4,
            "memory_bytes": 8_000_000_000,
            "runner_image_digest": "sha256:test",
        },
        "scale": {"clients": 100, "tunnels": 2_000, "concurrent_relays": 10_000},
        "baseline": {"payload_throughput_bits_per_second": 1_300_000_000.0},
        "minitun": {
            "connections_attempted": 10_000,
            "connections_succeeded": 10_000,
            "connections_failed": 0,
            "data_corruption": 0,
            "payload_throughput_bits_per_second": 1_100_000_000.0,
            "first_byte_latency_ms": {"p50": 50.0, "p95": 200.0, "p99": 220.0},
        },
        "throughput_ratio": 0.9,
        "restart_convergence_seconds": 20.0,
        "peak_minitun_rss_bytes": 3_000_000_000,
        "stable_rss_drift_fraction": 0.01,
        "soak_seconds": 0,
        "failures": [],
    }


def performance() -> dict:
    runs = [base_run() for _ in range(3)]
    return {
        "evidence_format": 1,
        "source_commit": SHA,
        "started_at": "2026-08-01T00:00:00Z",
        "finished_at": "2026-08-01T00:10:00Z",
        "independent_runs": 3,
        "median_payload_throughput_bits_per_second": 1_100_000_000.0,
        "median_throughput_ratio": 0.9,
        "runs": runs,
        "failures": [],
    }


def soak(phase: str, started: str) -> dict:
    document = base_run()
    seconds = VERIFIER.PHASE_SECONDS[phase]
    start = VERIFIER.timestamp(started, "test.started")
    finish = start + VERIFIER.dt.timedelta(seconds=seconds)
    document.update({
        "soak_started_at": start.isoformat().replace("+00:00", "Z"),
        "soak_finished_at": finish.isoformat().replace("+00:00", "Z"),
        "soak_elapsed_seconds": seconds,
        "soak_cycles": 5,
        "soak_events_enabled": phase == "mixed-7d",
        "soak_seconds": seconds,
    })
    return document


class EvidenceVerifierTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, name: str, document: dict) -> Path:
        path = self.root / name
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def test_accepts_complete_performance_evidence(self) -> None:
        result = VERIFIER.validate_performance(self.write("performance.json", performance()), SHA)
        self.assertEqual(result["independent_runs"], 3)

    def test_median_threshold_controls_per_run_throughput(self) -> None:
        document = performance()
        document["runs"][0]["failures"] = ["throughput_below_1_gbit"]
        document["runs"][0]["minitun"]["payload_throughput_bits_per_second"] = 900_000_000
        VERIFIER.validate_performance(self.write("performance.json", document), SHA)

    def test_rejects_wrong_commit(self) -> None:
        with self.assertRaisesRegex(VERIFIER.EvidenceError, "source_commit"):
            VERIFIER.validate_performance(self.write("performance.json", performance()), "b" * 40)

    def test_rejects_shortened_soak_even_when_requested_field_is_forged(self) -> None:
        document = soak("full-24h", "2026-08-02T00:00:00Z")
        document["soak_elapsed_seconds"] = 60
        document["soak_finished_at"] = "2026-08-02T00:01:00Z"
        with self.assertRaisesRegex(VERIFIER.EvidenceError, "duration is too short"):
            VERIFIER.validate_soak(self.write("full.json", document), SHA, "full-24h")

    def test_rejects_mixed_soak_that_precedes_full_soak(self) -> None:
        perf_path = self.write("performance.json", performance())
        full_path = self.write("full.json", soak("full-24h", "2026-08-10T00:00:00Z"))
        mixed_path = self.write("mixed.json", soak("mixed-7d", "2026-08-09T00:00:00Z"))
        arguments = type("Arguments", (), {
            "tag": "v1.0.0",
            "source_sha": SHA,
            "performance": perf_path,
            "full_24h": full_path,
            "mixed_7d": mixed_path,
            "not_before": "2026-08-08T00:00:00Z",
        })()
        with self.assertRaisesRegex(VERIFIER.EvidenceError, "must start after"):
            VERIFIER.validate_release(arguments)

    def test_accepts_ordered_ga_evidence(self) -> None:
        perf_path = self.write("performance.json", performance())
        full_path = self.write("full.json", soak("full-24h", "2026-08-10T00:00:00Z"))
        mixed_path = self.write("mixed.json", soak("mixed-7d", "2026-08-11T00:00:00Z"))
        arguments = type("Arguments", (), {
            "tag": "v1.0.0",
            "source_sha": SHA,
            "performance": perf_path,
            "full_24h": full_path,
            "mixed_7d": mixed_path,
            "not_before": "2026-08-09T00:00:00Z",
        })()
        result = VERIFIER.validate_release(arguments)
        self.assertEqual(result["release_kind"], "general-availability")


if __name__ == "__main__":
    unittest.main()
