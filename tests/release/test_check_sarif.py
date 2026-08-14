#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path

SOURCE_ROOT = Path(os.environ["MINITUN_SOURCE_DIR"])
SPEC = importlib.util.spec_from_file_location("check_sarif", SOURCE_ROOT / "ci/check_sarif.py")
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


def document(severities: dict[str, str]) -> dict:
    return {
        "version": "2.1.0",
        "runs": [{
            "tool": {"driver": {"name": "test", "rules": [
                {"id": identifier, "properties": {"security-severity": severity}}
                for identifier, severity in severities.items()
            ]}},
            "results": [
                {"ruleId": identifier, "message": {"text": identifier}}
                for identifier in severities
            ],
        }],
    }


class SarifGateTest(unittest.TestCase):
    def test_accepts_findings_below_high(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "result.sarif"
            path.write_text(json.dumps(document({"warning": "6.9"})), encoding="utf-8")
            self.assertEqual(CHECKER.scan([path], 7.0), [])

    def test_rejects_high_and_critical_findings(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "result.sarif"
            path.write_text(json.dumps(document({"high": "7.0", "critical": "9.8"})),
                            encoding="utf-8")
            findings = CHECKER.scan([path], 7.0)
            self.assertEqual([finding[1] for finding in findings], ["high", "critical"])


if __name__ == "__main__":
    unittest.main()
