from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.ctest_repeat_evidence.v1"
INVENTORY_SCHEMA = "gamenet.ctest_inventory.v1"
RESULT_LINE = re.compile(
    r"^\s*(?:\d+/\d+\s+)?Test\s+#?\d+:\s+(?P<name>\S+)\s+.*$"
)
PASSED_RESULT = re.compile(r"\sPassed\s+[0-9]+(?:\.[0-9]+)?\s+sec\s*$")
TOTAL_TIME = re.compile(r"Total Test time \(real\) =\s*([0-9]+(?:\.[0-9]+)?)\s*sec")


class RepeatEvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RepeatEvidenceError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_inventory(path: Path) -> list[dict[str, Any]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RepeatEvidenceError(f"cannot read CTest inventory {path}: {error}") from error
    require(document.get("schema") == INVENTORY_SCHEMA, "unexpected CTest inventory schema")
    tests = document.get("tests")
    require(isinstance(tests, list), "CTest inventory must contain a tests array")
    normalized: list[dict[str, Any]] = []
    names: set[str] = set()
    for test in tests:
        require(isinstance(test, dict), "CTest inventory contains a non-object test")
        name = test.get("name")
        labels = test.get("labels")
        require(isinstance(name, str) and bool(name), "CTest inventory contains an unnamed test")
        require(name not in names, f"duplicate CTest inventory name: {name}")
        require(
            isinstance(labels, list) and all(isinstance(label, str) for label in labels),
            f"CTest inventory labels must be strings: {name}",
        )
        names.add(name)
        normalized.append({"name": name, "labels": labels})
    return normalized


def selected_names(tests: list[dict[str, Any]], labels: set[str]) -> list[str]:
    return sorted(
        test["name"]
        for test in tests
        if labels.intersection(test["labels"])
    )


def parse_log(path: Path) -> tuple[Counter[str], list[str], float, str]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise RepeatEvidenceError(f"cannot read CTest repeat log {path}: {error}") from error

    passed: Counter[str] = Counter()
    nonpassing: list[str] = []
    for line in text.splitlines():
        match = RESULT_LINE.match(line)
        if match is None:
            continue
        name = match.group("name")
        if PASSED_RESULT.search(line) is None:
            nonpassing.append(line.strip())
        else:
            passed[name] += 1

    time_matches = TOTAL_TIME.findall(text)
    require(len(time_matches) == 1, "CTest repeat log must contain exactly one total real time")
    return passed, nonpassing, float(time_matches[0]), text


def verify_repeat(
    inventory_path: Path,
    log_path: Path,
    labels: set[str],
    expected_selected: int,
    repeat: int,
    timeout_seconds: int,
    command: str,
) -> dict[str, Any]:
    require(labels, "at least one selection label is required")
    require(expected_selected > 0, "expected selected count must be positive")
    require(repeat > 0, "repeat count must be positive")
    require(timeout_seconds > 0, "timeout must be positive")
    require(bool(command.strip()), "repeat command must not be empty")

    tests = load_inventory(inventory_path)
    expected_names = selected_names(tests, labels)
    require(
        len(expected_names) == expected_selected,
        f"selected CTest inventory drift: expected {expected_selected}, got {len(expected_names)}",
    )

    passed, nonpassing, total_seconds, text = parse_log(log_path)
    require(not nonpassing, "CTest repeat log contains non-passing result lines: " + " | ".join(nonpassing))
    unexpected = sorted(set(passed) - set(expected_names))
    missing = sorted(set(expected_names) - set(passed))
    require(not unexpected, f"repeat log contains tests outside the selected inventory: {unexpected}")
    require(not missing, f"repeat log is missing selected tests: {missing}")
    wrong_counts = {
        name: passed[name]
        for name in expected_names
        if passed[name] != repeat
    }
    require(not wrong_counts, f"repeat execution count mismatch: {wrong_counts}")

    total_executions = expected_selected * repeat
    require(
        sum(passed.values()) == total_executions,
        f"repeat execution total mismatch: expected {total_executions}, got {sum(passed.values())}",
    )
    summary = f"100% tests passed, 0 tests failed out of {expected_selected}"
    require(text.count(summary) == 1, f"CTest repeat log lacks the exact success summary: {summary}")

    generated_at = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    return {
        "schema": SCHEMA,
        "generated_at_utc": generated_at,
        "result": "success",
        "selection": {
            "match": "any-label",
            "labels": sorted(labels),
            "selected_tests": expected_selected,
        },
        "repeat": repeat,
        "timeout_seconds": timeout_seconds,
        "expected_executions": total_executions,
        "actual_executions": sum(passed.values()),
        "total_test_time_seconds": total_seconds,
        "command": command,
        "inventory": {
            "file": inventory_path.name,
            "sha256": sha256_file(inventory_path),
        },
        "log": {
            "file": log_path.name,
            "bytes": log_path.stat().st_size,
            "sha256": sha256_file(log_path),
        },
        "tests": [
            {"name": name, "executions": passed[name]}
            for name in expected_names
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify every selected CTest passed an exact repeat-until-fail count."
    )
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--selection-label", action="append", required=True)
    parser.add_argument("--expected-selected", type=int, required=True)
    parser.add_argument("--repeat", type=int, required=True)
    parser.add_argument("--timeout-seconds", type=int, required=True)
    parser.add_argument("--command", required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    try:
        evidence = verify_repeat(
            arguments.inventory,
            arguments.log,
            set(arguments.selection_label),
            arguments.expected_selected,
            arguments.repeat,
            arguments.timeout_seconds,
            arguments.command,
        )
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    except (RepeatEvidenceError, OSError) as error:
        print(f"CTest repeat evidence verification failed: {error}", file=sys.stderr)
        return 1

    print(
        f"verified {evidence['selection']['selected_tests']} tests x {evidence['repeat']} repeats "
        f"= {evidence['actual_executions']} passing executions"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
