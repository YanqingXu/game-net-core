from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.phase4_benchmark.v1"
MANIFEST_SCHEMA = "gamenet.phase4_benchmark_evidence.v1"
SCENARIOS = ("framing", "logic-queue", "broadcast-fanout")


class BenchmarkValidationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BenchmarkValidationError(message)


def require_object(value: Any, label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be a JSON object")
    return value


def require_positive_number(value: Any, label: str) -> float:
    require(
        not isinstance(value, bool) and isinstance(value, (int, float)),
        f"{label} must be a finite positive number",
    )
    numeric = float(value)
    require(math.isfinite(numeric) and numeric > 0.0, f"{label} must be a finite positive number")
    return numeric


def require_nonnegative_number(value: Any, label: str) -> float:
    require(
        not isinstance(value, bool) and isinstance(value, (int, float)),
        f"{label} must be a finite non-negative number",
    )
    numeric = float(value)
    require(
        math.isfinite(numeric) and numeric >= 0.0,
        f"{label} must be a finite non-negative number",
    )
    return numeric


def require_positive_count(value: Any, label: str) -> int:
    require(not isinstance(value, bool) and isinstance(value, int), f"{label} must be a positive integer")
    require(value > 0, f"{label} must be a positive integer")
    return value


def require_nonnegative_count(value: Any, label: str) -> int:
    require(
        not isinstance(value, bool) and isinstance(value, int),
        f"{label} must be a non-negative integer",
    )
    require(value >= 0, f"{label} must be a non-negative integer")
    return value


def require_percentiles(measurements: dict[str, Any], prefix: str, label: str) -> None:
    p50 = require_nonnegative_number(measurements.get(f"{prefix}_p50_us"), f"{label} P50")
    p99 = require_nonnegative_number(measurements.get(f"{prefix}_p99_us"), f"{label} P99")
    require(p99 >= p50, f"{label} P99 must be greater than or equal to P50")


def require_operation_rate(
    measurements: dict[str, Any],
    elapsed_ms: float,
    operation_count: int,
    label: str,
) -> float:
    observed = require_positive_number(measurements.get("operations_per_second"), f"{label} operations/s")
    expected = operation_count * 1000.0 / elapsed_ms
    require(
        math.isclose(observed, expected, rel_tol=0.001, abs_tol=0.05),
        f"{label} operations/s does not match its elapsed-time and operation-count invariant",
    )
    return observed


def validate_framing(
    parameters: dict[str, Any], measurements: dict[str, Any], elapsed_ms: float
) -> None:
    messages = require_positive_count(parameters.get("messages"), "framing messages")
    payload_bytes = require_positive_count(parameters.get("payload_bytes"), "framing payload bytes")
    require_positive_count(parameters.get("batch_size"), "framing batch size")
    operations_per_second = require_operation_rate(measurements, elapsed_ms, messages, "framing")
    throughput = require_positive_number(
        measurements.get("throughput_mib_per_second"), "framing throughput MiB/s"
    )
    expected_throughput = operations_per_second * (payload_bytes + 4) / (1024.0 * 1024.0)
    require(
        math.isclose(throughput, expected_throughput, rel_tol=0.001, abs_tol=0.05),
        "framing throughput does not match wire bytes and decoded operations",
    )


def validate_logic_queue(
    parameters: dict[str, Any], measurements: dict[str, Any], elapsed_ms: float
) -> None:
    messages = require_positive_count(parameters.get("messages"), "logic messages")
    payload_bytes = require_positive_count(parameters.get("payload_bytes"), "logic payload bytes")
    require_positive_count(parameters.get("threads"), "logic producer threads")
    require_positive_count(parameters.get("batch_size"), "logic batch size")
    require_positive_count(parameters.get("tick_interval_us"), "logic tick interval")
    require_operation_rate(measurements, elapsed_ms, messages, "logic queue")
    require_percentiles(measurements, "logic_queue_lag", "logic queue lag")

    depth_high_water = require_positive_count(
        measurements.get("logic_depth_high_watermark"), "logic depth high water"
    )
    bytes_high_water = require_positive_count(
        measurements.get("logic_bytes_high_watermark"), "logic bytes high water"
    )
    accepted = require_nonnegative_count(measurements.get("logic_accepted"), "logic accepted")
    rejected = require_nonnegative_count(measurements.get("logic_rejected"), "logic rejected")
    require(accepted == messages, "logic accepted count must equal configured messages")
    require(rejected == 0, "logic rejected count must be zero for a successful baseline")
    require(depth_high_water <= messages, "logic depth high water cannot exceed configured messages")
    require(
        bytes_high_water == depth_high_water * payload_bytes,
        "logic byte high water must match fixed payload bytes times depth high water",
    )


def tasks_per_broadcast_iteration(fanout: int, threads: int, batch_size: int) -> int:
    base, extra = divmod(fanout, threads)
    return sum(
        (base + (1 if owner < extra else 0) + batch_size - 1) // batch_size
        for owner in range(threads)
        if base + (1 if owner < extra else 0) > 0
    )


def validate_broadcast_fanout(
    parameters: dict[str, Any], measurements: dict[str, Any], elapsed_ms: float
) -> None:
    messages = require_positive_count(parameters.get("messages"), "broadcast messages")
    fanout = require_positive_count(parameters.get("fanout"), "broadcast fanout")
    threads = require_positive_count(parameters.get("threads"), "broadcast owner threads")
    batch_size = require_positive_count(parameters.get("batch_size"), "broadcast batch size")
    require_positive_count(parameters.get("payload_bytes"), "broadcast payload bytes")

    endpoint_operations = messages * fanout
    require_operation_rate(measurements, elapsed_ms, endpoint_operations, "broadcast fanout")
    for prefix, label in (
        ("broadcast_endpoint_latency", "broadcast endpoint latency"),
        ("broadcast_route_latency", "broadcast route latency"),
        ("broadcast_queue_latency", "broadcast queue latency"),
        ("broadcast_fanout_completion", "broadcast fanout completion"),
    ):
        require_percentiles(measurements, prefix, label)

    accepted = require_nonnegative_count(measurements.get("broadcast_accepted"), "broadcast accepted")
    dropped = require_nonnegative_count(measurements.get("broadcast_dropped"), "broadcast dropped")
    scheduled_tasks = require_positive_count(
        measurements.get("broadcast_scheduled_tasks"), "broadcast scheduled tasks"
    )
    task_high_water = require_positive_count(
        measurements.get("broadcast_task_high_watermark"), "broadcast task high water"
    )
    expected_tasks_per_iteration = tasks_per_broadcast_iteration(fanout, threads, batch_size)
    require(accepted == endpoint_operations, "broadcast accepted count must equal messages times fanout")
    require(dropped == 0, "broadcast dropped count must be zero for a successful baseline")
    require(
        scheduled_tasks == messages * expected_tasks_per_iteration,
        "broadcast scheduled-task count does not match owner grouping and batch size",
    )
    require(
        task_high_water == expected_tasks_per_iteration,
        "broadcast task high water must equal one iteration's scheduled task count",
    )

    before = require_positive_count(measurements.get("working_set_before_bytes"), "working set before")
    after = require_positive_count(measurements.get("working_set_after_bytes"), "working set after")
    peak = require_positive_count(measurements.get("working_set_peak_bytes"), "working set peak")
    peak_delta = require_nonnegative_count(
        measurements.get("working_set_peak_delta_bytes"), "working set peak delta"
    )
    require(peak >= before and peak >= after, "working set peak must cover before and after samples")
    require(peak_delta == peak - before, "working set peak delta must equal peak minus before")


def validate_document(
    document: Any,
    expected_scenario: str,
    expected_platform: str,
    expected_backend: str,
    expected_build_type: str,
    source: Path,
) -> dict[str, Any]:
    root = require_object(document, str(source))
    require(root.get("schema") == SCHEMA, f"{source}: unexpected schema")
    require(root.get("status") == "ok", f"{source}: status is not ok: {root.get('error')!r}")
    require(root.get("error") is None, f"{source}: successful result must have null error")
    require(root.get("scenario") == expected_scenario, f"{source}: scenario does not match its file")
    require(root.get("platform") == expected_platform, f"{source}: unexpected platform")
    require(root.get("backend") == expected_backend, f"{source}: unexpected backend")
    require(root.get("build_type") == expected_build_type, f"{source}: unexpected build type")

    parameters = require_object(root.get("parameters"), f"{source}: parameters")
    for key in (
        "messages",
        "payload_bytes",
        "threads",
        "batch_size",
        "fanout",
        "tick_interval_us",
        "timeout_ms",
    ):
        require_positive_count(parameters.get(key), f"{source}: parameter {key}")
    measurements = require_object(root.get("measurements"), f"{source}: measurements")
    elapsed_ms = require_positive_number(measurements.get("elapsed_ms"), f"{source}: elapsed milliseconds")

    if expected_scenario == "framing":
        validate_framing(parameters, measurements, elapsed_ms)
    elif expected_scenario == "logic-queue":
        validate_logic_queue(parameters, measurements, elapsed_ms)
    else:
        validate_broadcast_fanout(parameters, measurements, elapsed_ms)
    return root


def validate_directory(
    input_dir: Path,
    platform: str,
    backend: str,
    build_type: str = "Release",
) -> dict[str, tuple[Path, dict[str, Any]]]:
    require(input_dir.is_dir(), f"benchmark input directory does not exist: {input_dir}")
    expected_names = {f"{scenario}.json" for scenario in SCENARIOS}
    actual_names = {
        path.name
        for path in input_dir.glob("*.json")
        if path.name != "evidence-manifest.json"
    }
    require(
        actual_names == expected_names,
        f"benchmark directory must contain exactly {sorted(expected_names)}, got {sorted(actual_names)}",
    )

    validated: dict[str, tuple[Path, dict[str, Any]]] = {}
    for scenario in SCENARIOS:
        path = input_dir / f"{scenario}.json"
        try:
            document = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise BenchmarkValidationError(f"failed to read benchmark JSON {path}: {error}") from error
        validated[scenario] = (
            path,
            validate_document(document, scenario, platform, backend, build_type, path),
        )
    return validated


def write_manifest(
    output: Path,
    validated: dict[str, tuple[Path, dict[str, Any]]],
    platform: str,
    backend: str,
    build_type: str,
    commit_sha: str,
    run_id: str,
    job: str,
    artifact_name: str,
) -> None:
    require(re.fullmatch(r"[0-9a-fA-F]{40}", commit_sha) is not None, "commit SHA must contain 40 hex digits")
    require(re.fullmatch(r"[0-9]+", run_id) is not None, "run id must be an unsigned decimal integer")
    require(bool(job.strip()), "job name must not be empty")
    require(bool(artifact_name.strip()), "artifact name must not be empty")

    artifacts = []
    for scenario in SCENARIOS:
        path, document = validated[scenario]
        artifacts.append(
            {
                "scenario": scenario,
                "file": path.name,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "parameters": document["parameters"],
            }
        )
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "commit_sha": commit_sha.lower(),
        "run_id": run_id,
        "job": job,
        "artifact_name": artifact_name,
        "platform": platform,
        "backend": backend,
        "build_type": build_type,
        "generated_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "validation": "semantic metrics and count invariants; no performance thresholds",
        "artifacts": artifacts,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate Phase 4 benchmark evidence JSON")
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--backend", choices=("epoll", "iocp"), required=True)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--manifest-output", type=Path)
    parser.add_argument("--commit-sha")
    parser.add_argument("--run-id")
    parser.add_argument("--job")
    parser.add_argument("--artifact-name")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        validated = validate_directory(args.input_dir, args.platform, args.backend, args.build_type)
        if args.manifest_output is not None:
            metadata = (args.commit_sha, args.run_id, args.job, args.artifact_name)
            require(
                all(value is not None for value in metadata),
                "manifest output requires SHA, run id, job, and artifact name",
            )
            write_manifest(
                args.manifest_output,
                validated,
                args.platform,
                args.backend,
                args.build_type,
                args.commit_sha,
                args.run_id,
                args.job,
                args.artifact_name,
            )
    except BenchmarkValidationError as error:
        print(f"Phase 4 benchmark validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
