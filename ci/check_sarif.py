#!/usr/bin/env python3
"""Fail when SARIF contains a security finding at or above a severity threshold."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def rule_map(run: dict[str, Any]) -> dict[str, dict[str, Any]]:
    tools = run.get("tool", {})
    components = [tools.get("driver", {})]
    components.extend(tools.get("extensions", []))
    rules: dict[str, dict[str, Any]] = {}
    for component in components:
        for rule in component.get("rules", []):
            identifier = rule.get("id")
            if isinstance(identifier, str):
                rules[identifier] = rule
    return rules


def security_severity(rule: dict[str, Any]) -> float | None:
    value = rule.get("properties", {}).get("security-severity")
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def scan(paths: list[Path], threshold: float) -> list[tuple[Path, str, float, str]]:
    findings: list[tuple[Path, str, float, str]] = []
    for path in paths:
        document = json.loads(path.read_text(encoding="utf-8"))
        for run in document.get("runs", []):
            rules = rule_map(run)
            for result in run.get("results", []):
                rule_id = result.get("ruleId", "unknown")
                severity = security_severity(rules.get(rule_id, {}))
                if severity is None or severity < threshold:
                    continue
                message = result.get("message", {}).get("text", "security finding")
                findings.append((path, str(rule_id), severity, str(message).replace("\n", " ")))
    return findings


def sarif_files(root: Path) -> list[Path]:
    if root.is_file():
        return [root]
    return sorted(path for path in root.rglob("*") if path.suffix in {".sarif", ".json"})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--threshold", type=float, default=7.0)
    args = parser.parse_args()
    paths = sarif_files(args.path)
    if not paths:
        print(f"no SARIF files found under {args.path}", file=sys.stderr)
        return 2
    findings = scan(paths, args.threshold)
    for path, rule_id, severity, message in findings:
        print(f"{path}: {rule_id} security-severity={severity:g}: {message}", file=sys.stderr)
    if findings:
        print(f"rejected {len(findings)} high/critical security finding(s)", file=sys.stderr)
        return 1
    print(f"accepted {len(paths)} SARIF file(s): no security-severity >= {args.threshold:g}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
