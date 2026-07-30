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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_core_benchmark import (  # noqa: E402
    CoreBenchmarkValidationError,
    validate_document as validate_core_document,
)


SCHEMA = "gamenet.core_accept_topology_decision.v1"
MATRIX_SCHEMA = "gamenet.performance_matrix.v1"
MATRIX_PROFILE = "core-capacity"
CHURN_KEY = "core.connection-churn-1000"
REPETITIONS = 3
RATE_FLOOR_RATIO = 0.95
ACCEPT_CADENCE_SATURATION_RATIO = 0.5


class TopologyDecisionError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TopologyDecisionError(message)


def read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise TopologyDecisionError(f"cannot read {label} {path}: {error}") from error
    require(isinstance(document, dict), f"{label} must be a JSON object: {path}")
    return document


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_sample_path(root: Path, relative: Any) -> Path:
    require(isinstance(relative, str) and relative, "churn sample path is missing")
    pure = PurePosixPath(relative)
    require(
        not pure.is_absolute() and ".." not in pure.parts,
        f"unsafe churn sample path: {relative}",
    )
    path = root.joinpath(*pure.parts)
    require(path.is_file(), f"churn sample is missing: {path}")
    return path


def finite_metric(document: dict[str, Any], name: str) -> float:
    value = document["measurements"].get(name)
    require(
        not isinstance(value, bool) and isinstance(value, (int, float)),
        f"{name} must be numeric",
    )
    numeric = float(value)
    require(
        math.isfinite(numeric) and numeric >= 0.0,
        f"{name} must be finite and non-negative",
    )
    return numeric


def evaluate(input_root: Path, candidate_sha: str) -> dict[str, Any]:
    require(
        re.fullmatch(r"[0-9a-f]{40}", candidate_sha) is not None,
        "candidate SHA must be 40 lowercase hex digits",
    )
    manifest_path = input_root / "matrix-manifest.json"
    manifest = read_json(manifest_path, "capacity matrix manifest")
    require(manifest.get("schema") == MATRIX_SCHEMA, "capacity matrix schema mismatch")
    require(manifest.get("profile") == MATRIX_PROFILE, "capacity matrix profile mismatch")
    require(manifest.get("commit_sha") == candidate_sha, "capacity matrix commit mismatch")
    require(manifest.get("platform") == "linux", "accept topology requires Linux evidence")
    require(manifest.get("backend") == "epoll", "accept topology requires epoll evidence")
    require(manifest.get("build_type") == "Release", "accept topology requires Release evidence")
    require(manifest.get("repetitions") == REPETITIONS, "capacity repetition count mismatch")

    scenarios = manifest.get("scenarios")
    require(isinstance(scenarios, list), "capacity scenarios must be an array")
    matches = [
        scenario
        for scenario in scenarios
        if isinstance(scenario, dict) and scenario.get("key") == CHURN_KEY
    ]
    require(len(matches) == 1, f"capacity matrix must contain exactly one {CHURN_KEY}")
    scenario = matches[0]
    parameters = scenario.get("parameters")
    require(isinstance(parameters, dict), "churn scenario parameters are missing")
    require(parameters.get("connections") == 100, "churn batch size must be 100")
    require(parameters.get("event_loop_threads") == 4, "churn worker count must be 4")
    require(parameters.get("churn_target_per_second") == 1000, "churn target must be 1000/s")
    require(parameters.get("churn_duration_ms") == 5000, "churn duration must be 5000 ms")
    require(
        parameters.get("churn_connect_timeout_ms") == 1000,
        "churn connect deadline must be 1000 ms",
    )

    samples = scenario.get("samples")
    require(
        isinstance(samples, list) and len(samples) == REPETITIONS,
        "churn sample count mismatch",
    )
    documents = []
    retained_samples = []
    for index, sample in enumerate(samples, start=1):
        require(isinstance(sample, dict), "churn sample metadata must be an object")
        path = safe_sample_path(input_root, sample.get("path"))
        digest = sha256_file(path)
        require(sample.get("sha256") == digest, f"churn sample hash mismatch: {path}")
        document = read_json(path, f"churn sample {index}")
        try:
            validate_core_document(
                document,
                expected_platform="linux",
                expected_backend="epoll",
                expected_build_type="Release",
                expected_scenario="connection-churn",
                require_zero_churn_failures=True,
                label=f"{CHURN_KEY} sample {index}",
            )
        except CoreBenchmarkValidationError as error:
            raise TopologyDecisionError(str(error)) from error
        require(
            document.get("parameters") == parameters,
            "churn sample parameters differ from the matrix manifest",
        )
        documents.append(document)
        retained_samples.append({"path": sample["path"], "sha256": digest})

    metric_names = (
        "churn_attempts_per_second",
        "churn_connect_p99_us",
        "churn_accept_p99_us",
        "churn_close_p99_us",
        "churn_schedule_lag_p99_us",
    )
    medians = {
        name: float(statistics.median(finite_metric(document, name) for document in documents))
        for name in metric_names
    }
    target_rate = float(parameters["churn_target_per_second"])
    batch_cadence_us = (
        float(parameters["connections"]) / target_rate * 1_000_000.0
    )
    actual_rate_ratio = medians["churn_attempts_per_second"] / target_rate
    rate_missed = actual_rate_ratio < RATE_FLOOR_RATIO
    accept_dominant = (
        medians["churn_accept_p99_us"] >= medians["churn_connect_p99_us"]
        and medians["churn_accept_p99_us"] >= medians["churn_close_p99_us"]
    )
    accept_saturated = (
        medians["churn_accept_p99_us"]
        >= batch_cadence_us * ACCEPT_CADENCE_SATURATION_RATIO
    )
    trigger_experiment = rate_missed and accept_dominant and accept_saturated

    return {
        "schema": SCHEMA,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "result": "experiment_so_reuseport" if trigger_experiment else "retain_single_listener",
        "candidate_sha": candidate_sha,
        "platform": "linux",
        "backend": "epoll",
        "build_type": "Release",
        "matrix_profile": MATRIX_PROFILE,
        "scenario_key": CHURN_KEY,
        "repetitions": REPETITIONS,
        "parameters": parameters,
        "samples": retained_samples,
        "medians": medians,
        "criteria": {
            "rate_floor_ratio": RATE_FLOOR_RATIO,
            "accept_cadence_saturation_ratio": ACCEPT_CADENCE_SATURATION_RATIO,
            "batch_cadence_us": batch_cadence_us,
            "actual_rate_ratio": actual_rate_ratio,
            "rate_missed": rate_missed,
            "accept_dominant": accept_dominant,
            "accept_saturated": accept_saturated,
            "trigger_experiment": trigger_experiment,
        },
        "matrix_manifest_sha256": sha256_file(manifest_path),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Evaluate whether Linux sustained churn justifies a SO_REUSEPORT experiment."
    )
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        output = evaluate(args.input_root, args.candidate_sha)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    except (TopologyDecisionError, OSError) as error:
        print(f"Core accept-topology decision failed: {error}", file=sys.stderr)
        return 1
    print(f"Core accept-topology decision: {output['result']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
