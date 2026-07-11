from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.ctest_inventory.v1"


class InventoryError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise InventoryError(message)


def parse_label_expectation(value: str) -> tuple[str, int]:
    label, separator, raw_count = value.partition("=")
    require(bool(separator) and bool(label), f"invalid label expectation: {value!r}")
    try:
        count = int(raw_count, 10)
    except ValueError as error:
        raise InventoryError(f"invalid label count: {value!r}") from error
    require(count >= 0, f"label count must be non-negative: {value!r}")
    return label, count


def test_labels(test: dict[str, Any]) -> list[str]:
    labels: list[str] = []
    for prop in test.get("properties", []):
        if prop.get("name") != "LABELS":
            continue
        value = prop.get("value", [])
        if isinstance(value, str):
            labels.extend(item for item in value.split(";") if item)
        elif isinstance(value, list):
            labels.extend(str(item) for item in value)
    return sorted(set(labels))


def load_inventory(test_dir: Path, config: str | None) -> tuple[list[str], dict[str, list[str]], list[str]]:
    command = ["ctest", "--test-dir", str(test_dir)]
    if config:
        command.extend(["-C", config])
    command.append("--show-only=json-v1")
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    require(result.returncode == 0, f"CTest inventory command failed: {result.stderr.strip()}")
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise InventoryError(f"CTest did not emit valid JSON: {error}") from error
    tests = document.get("tests")
    require(isinstance(tests, list), "CTest inventory JSON has no tests array")

    names: list[str] = []
    labels_by_test: dict[str, list[str]] = {}
    for test in tests:
        require(isinstance(test, dict), "CTest inventory contains a non-object test")
        name = test.get("name")
        require(isinstance(name, str) and bool(name), "CTest inventory contains an unnamed test")
        require(name not in labels_by_test, f"duplicate CTest name: {name}")
        names.append(name)
        labels_by_test[name] = test_labels(test)
    return names, labels_by_test, command


def write_evidence(
    output: Path,
    names: list[str],
    labels_by_test: dict[str, list[str]],
    command: list[str],
    expected_total: int,
    expectations: dict[str, int],
) -> None:
    label_counts = {
        label: sum(label in labels_by_test[name] for name in names)
        for label in sorted({item for labels in labels_by_test.values() for item in labels})
    }
    evidence = {
        "schema": SCHEMA,
        "generated_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "command": command,
        "total": len(names),
        "expected_total": expected_total,
        "label_counts": label_counts,
        "expected_label_counts": expectations,
        "tests": [
            {"name": name, "labels": labels_by_test[name]}
            for name in names
        ],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify and record an exact CTest inventory")
    parser.add_argument("--test-dir", type=Path, required=True)
    parser.add_argument("--config")
    parser.add_argument("--expected-total", type=int, required=True)
    parser.add_argument("--expect-label", action="append", default=[], metavar="LABEL=COUNT")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()

    try:
        require(arguments.expected_total >= 0, "expected total must be non-negative")
        expectations = dict(parse_label_expectation(item) for item in arguments.expect_label)
        require(
            len(expectations) == len(arguments.expect_label),
            "each expected label may be named only once",
        )
        names, labels_by_test, command = load_inventory(arguments.test_dir, arguments.config)
        require(
            len(names) == arguments.expected_total,
            f"CTest inventory drift: expected {arguments.expected_total}, got {len(names)}",
        )
        for label, expected in expectations.items():
            actual = sum(label in labels_by_test[name] for name in names)
            require(
                actual == expected,
                f"CTest label inventory drift for {label!r}: expected {expected}, got {actual}",
            )
        if arguments.output is not None:
            write_evidence(
                arguments.output,
                names,
                labels_by_test,
                command,
                arguments.expected_total,
                expectations,
            )
    except InventoryError as error:
        print(f"CTest inventory verification failed: {error}", file=sys.stderr)
        return 1

    print(
        f"verified {len(names)} CTest entries"
        + "".join(f", {label}={count}" for label, count in sorted(expectations.items()))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
