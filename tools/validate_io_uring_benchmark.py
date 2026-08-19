#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.io_uring_one_shot_benchmark.v1"


class IoUringBenchmarkValidationError(ValueError):
    pass


def fail(message: str) -> None:
    raise IoUringBenchmarkValidationError(message)


def positive_number(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail(f"{name} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        fail(f"{name} must be finite and positive")
    return result


def nonnegative_integer(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        fail(f"{name} must be a nonnegative integer")
    return value


def close(actual: float, expected: float) -> bool:
    return math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-9)


def validate_document(document: dict[str, Any], require_release: bool = False) -> None:
    if document.get("schema") != SCHEMA:
        fail("unexpected io_uring benchmark schema")
    if document.get("status") != "ok":
        fail("io_uring benchmark status is not ok")
    if require_release and document.get("build_type") != "Release":
        fail("io_uring benchmark is not a Release build")

    parameters = document.get("parameters")
    measurements = document.get("measurements")
    if not isinstance(parameters, dict) or not isinstance(measurements, dict):
        fail("io_uring benchmark parameters and measurements must be objects")

    round_trips = nonnegative_integer(parameters.get("round_trips"), "round_trips")
    payload_bytes = nonnegative_integer(parameters.get("payload_bytes"), "payload_bytes")
    depth = nonnegative_integer(parameters.get("depth"), "depth")
    if round_trips == 0 or payload_bytes == 0 or depth == 0 or depth > round_trips:
        fail("io_uring benchmark parameters violate finite positive bounds")

    elapsed = positive_number(measurements.get("elapsed_seconds"), "elapsed_seconds")
    messages_per_second = positive_number(
        measurements.get("messages_per_second"), "messages_per_second"
    )
    operations_per_second = positive_number(
        measurements.get("operations_per_second"), "operations_per_second"
    )
    throughput = positive_number(
        measurements.get("throughput_mib_per_second"), "throughput_mib_per_second"
    )
    if not close(messages_per_second, round_trips / elapsed):
        fail("message rate is inconsistent with round trips and elapsed time")
    if not close(operations_per_second, 2.0 * round_trips / elapsed):
        fail("operation rate is inconsistent with one Send and one Recv")
    expected_throughput = 2.0 * round_trips * payload_bytes / (1024.0 * 1024.0 * elapsed)
    if not close(throughput, expected_throughput):
        fail("throughput is inconsistent with round-trip bytes")

    p50 = nonnegative_integer(measurements.get("p50_latency_us"), "p50_latency_us")
    p99 = nonnegative_integer(measurements.get("p99_latency_us"), "p99_latency_us")
    p999 = nonnegative_integer(measurements.get("p999_latency_us"), "p999_latency_us")
    if not p50 <= p99 <= p999:
        fail("io_uring benchmark latency percentiles are not ordered")

    before = nonnegative_integer(
        measurements.get("working_set_before_bytes"), "working_set_before_bytes"
    )
    after = nonnegative_integer(
        measurements.get("working_set_after_bytes"), "working_set_after_bytes"
    )
    delta = measurements.get("working_set_delta_bytes")
    if isinstance(delta, bool) or not isinstance(delta, int) or delta != after - before:
        fail("working-set delta is inconsistent")
    shutdown_ms = measurements.get("shutdown_milliseconds")
    if isinstance(shutdown_ms, bool) or not isinstance(shutdown_ms, (int, float)):
        fail("shutdown_milliseconds must be numeric")
    if not math.isfinite(float(shutdown_ms)) or float(shutdown_ms) < 0:
        fail("shutdown_milliseconds must be finite and nonnegative")

    accepted = nonnegative_integer(
        measurements.get("operations_accepted"), "operations_accepted"
    )
    terminal = nonnegative_integer(
        measurements.get("terminal_notices"), "terminal_notices"
    )
    if accepted != 2 * round_trips:
        fail("operations accepted must equal twice round trips")
    if terminal != accepted:
        fail("terminal notices must equal accepted operations")
    if nonnegative_integer(
        measurements.get("sq_full_rejections"), "sq_full_rejections"
    ) != 0:
        fail("io_uring benchmark observed SQ-full rejection")
    if nonnegative_integer(
        measurements.get("cross_domain_fallbacks"), "cross_domain_fallbacks"
    ) != 0:
        fail("io_uring benchmark used a fallback")
    residual_fields = ("active_operations", "ready_notices", "owned_bytes")
    if any(nonnegative_integer(measurements.get(name), name) != 0 for name in residual_fields):
        fail("io_uring benchmark retained residual state")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate IOE-X1 benchmark JSON")
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--require-release", action="store_true")
    args = parser.parse_args()
    for path in args.paths:
        document = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            fail(f"{path}: root must be an object")
        validate_document(document, require_release=args.require_release)
        print(f"validated {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
