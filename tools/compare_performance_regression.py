#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any


BUDGET_SCHEMA = "gamenet.performance_regression_budget.v1"
MATRIX_SCHEMA = "gamenet.performance_matrix.v1"
OUTPUT_SCHEMA = "gamenet.performance_regression.v1"


class RegressionError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RegressionError(message)


def read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RegressionError(f"cannot read {label} {path}: {error}") from error
    require(isinstance(value, dict), f"{label} must be a JSON object: {path}")
    return value


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_sample_path(root: Path, relative: str) -> Path:
    pure = PurePosixPath(relative)
    require(not pure.is_absolute() and ".." not in pure.parts, f"unsafe sample path: {relative}")
    path = root.joinpath(*pure.parts)
    require(path.is_file(), f"sample file missing: {path}")
    return path


def finite_number(value: Any, label: str) -> float:
    require(not isinstance(value, bool) and isinstance(value, (int, float)), f"{label} must be numeric")
    numeric = float(value)
    require(math.isfinite(numeric) and numeric >= 0.0, f"{label} must be finite and non-negative")
    return numeric


def load_matrix(
    root: Path,
    expected_sha: str,
    platform: str,
    backend: str,
    repetitions: int,
) -> tuple[dict[str, Any], dict[str, tuple[dict[str, Any], list[dict[str, Any]]]]]:
    manifest = read_json(root / "matrix-manifest.json", "matrix manifest")
    require(manifest.get("schema") == MATRIX_SCHEMA, "matrix manifest schema mismatch")
    require(manifest.get("commit_sha") == expected_sha, "matrix commit SHA mismatch")
    require(manifest.get("platform") == platform, "matrix platform mismatch")
    require(manifest.get("backend") == backend, "matrix backend mismatch")
    require(manifest.get("build_type") == "Release", "matrix must use Release build")
    require(manifest.get("repetitions") == repetitions, "matrix repetition count mismatch")
    scenarios = manifest.get("scenarios")
    require(isinstance(scenarios, list), "matrix scenarios must be a list")
    loaded: dict[str, tuple[dict[str, Any], list[dict[str, Any]]]] = {}
    for item in scenarios:
        require(isinstance(item, dict), "matrix scenario entry must be an object")
        key = item.get("key")
        require(isinstance(key, str) and key not in loaded, f"invalid or duplicate matrix scenario: {key!r}")
        samples = item.get("samples")
        require(isinstance(samples, list) and len(samples) == repetitions, f"{key}: sample count mismatch")
        documents = []
        for sample in samples:
            require(isinstance(sample, dict), f"{key}: sample metadata must be an object")
            path = safe_sample_path(root, sample.get("path", ""))
            require(sample.get("sha256") == sha256_file(path), f"{key}: sample hash mismatch: {path}")
            document = read_json(path, f"{key} sample")
            require(document.get("status") == "ok", f"{key}: sample status is not ok")
            require(document.get("platform") == platform, f"{key}: sample platform mismatch")
            require(document.get("backend") == backend, f"{key}: sample backend mismatch")
            require(document.get("build_type") == "Release", f"{key}: sample build type mismatch")
            require(document.get("schema") == item.get("schema"), f"{key}: sample schema mismatch")
            require(document.get("scenario") == item.get("reported_scenario"), f"{key}: reported scenario mismatch")
            require(document.get("parameters") == item.get("parameters"), f"{key}: sample parameters mismatch")
            require(isinstance(document.get("measurements"), dict), f"{key}: measurements missing")
            documents.append(document)
        loaded[key] = (item, documents)
    return manifest, loaded


def compare(args: argparse.Namespace) -> tuple[dict[str, Any], bool]:
    budget = read_json(args.budget, "performance budget")
    require(budget.get("schema") == BUDGET_SCHEMA, "performance budget schema mismatch")
    require(re.fullmatch(r"[0-9a-f]{40}", args.baseline_sha) is not None, "baseline SHA must be 40 lowercase hex digits")
    require(re.fullmatch(r"[0-9a-f]{40}", args.candidate_sha) is not None, "candidate SHA must be 40 lowercase hex digits")
    require(budget.get("baseline_sha") == args.baseline_sha, "baseline SHA is not the reviewed budget baseline")
    repetitions = budget.get("repetitions")
    require(isinstance(repetitions, int) and repetitions >= 3 and repetitions % 2 == 1, "invalid budget repetitions")
    baseline_manifest, baseline = load_matrix(
        args.baseline_root, args.baseline_sha, args.platform, args.backend, repetitions
    )
    candidate_manifest, candidate = load_matrix(
        args.candidate_root, args.candidate_sha, args.platform, args.backend, repetitions
    )
    budget_scenarios = budget.get("scenarios")
    require(isinstance(budget_scenarios, list) and budget_scenarios, "performance budget scenarios missing")
    budget_keys = [item.get("key") for item in budget_scenarios if isinstance(item, dict)]
    require(len(budget_keys) == len(set(budget_keys)), "performance budget scenario keys must be unique")
    require(set(budget_keys) == set(baseline) == set(candidate), "budget and matrix scenario inventories differ")

    comparisons = []
    all_passed = True
    for scenario_budget in budget_scenarios:
        key = scenario_budget["key"]
        baseline_item, baseline_documents = baseline[key]
        candidate_item, candidate_documents = candidate[key]
        require(baseline_item.get("parameters") == candidate_item.get("parameters"), f"{key}: baseline/candidate parameters differ")
        metric_budgets = scenario_budget.get("metrics")
        require(isinstance(metric_budgets, list) and metric_budgets, f"{key}: metric budgets missing")
        metric_results = []
        for metric_budget in metric_budgets:
            require(isinstance(metric_budget, dict), f"{key}: metric budget must be an object")
            name = metric_budget.get("name")
            direction = metric_budget.get("direction")
            require(isinstance(name, str) and name, f"{key}: metric name missing")
            require(direction in {"higher", "lower"}, f"{key}.{name}: invalid direction")
            ratio = finite_number(metric_budget.get("max_regression_ratio"), f"{key}.{name} ratio")
            require(ratio < 1.0, f"{key}.{name}: max regression ratio must be below 1")
            slack = finite_number(metric_budget.get("absolute_slack"), f"{key}.{name} slack")
            baseline_values = [
                finite_number(document["measurements"].get(name), f"{key}.{name} baseline")
                for document in baseline_documents
            ]
            candidate_values = [
                finite_number(document["measurements"].get(name), f"{key}.{name} candidate")
                for document in candidate_documents
            ]
            baseline_median = float(statistics.median(baseline_values))
            candidate_median = float(statistics.median(candidate_values))
            if direction == "higher":
                threshold = baseline_median * (1.0 - ratio) - slack
                passed = candidate_median >= max(0.0, threshold)
            else:
                threshold = baseline_median * (1.0 + ratio) + slack
                passed = candidate_median <= threshold
            all_passed = all_passed and passed
            metric_results.append({
                "name": name,
                "direction": direction,
                "baseline_samples": baseline_values,
                "candidate_samples": candidate_values,
                "baseline_median": baseline_median,
                "candidate_median": candidate_median,
                "max_regression_ratio": ratio,
                "absolute_slack": slack,
                "threshold": threshold,
                "result": "pass" if passed else "fail",
            })
        comparisons.append({
            "key": key,
            "parameters": baseline_item["parameters"],
            "metrics": metric_results,
            "result": "pass" if all(item["result"] == "pass" for item in metric_results) else "fail",
        })

    output = {
        "schema": OUTPUT_SCHEMA,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "result": "pass" if all_passed else "fail",
        "platform": args.platform,
        "backend": args.backend,
        "build_type": "Release",
        "baseline_sha": args.baseline_sha,
        "candidate_sha": args.candidate_sha,
        "repetitions": repetitions,
        "baseline_executables": baseline_manifest.get("executables"),
        "candidate_executables": candidate_manifest.get("executables"),
        "budget_sha256": sha256_file(args.budget),
        "comparisons": comparisons,
    }
    return output, all_passed


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare same-runner GameNet performance matrices")
    parser.add_argument("--baseline-root", type=Path, required=True)
    parser.add_argument("--candidate-root", type=Path, required=True)
    parser.add_argument("--budget", type=Path, required=True)
    parser.add_argument("--baseline-sha", required=True)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        output, passed = compare(args)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    except (RegressionError, OSError) as error:
        print(f"performance regression comparison failed: {error}", file=sys.stderr)
        return 2
    if not passed:
        print(f"performance regression budget failed; evidence written to {args.output}", file=sys.stderr)
        return 1
    print(f"performance regression budget passed: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

