#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.io_uring_shared_tcp_hub_benchmark.v1"


class SharedHubBenchmarkValidationError(ValueError):
    pass


def fail(message: str) -> None:
    raise SharedHubBenchmarkValidationError(message)


def integer(value: Any, name: str, *, positive: bool = False) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        fail(f"{name} must be an integer")
    if value < (1 if positive else 0):
        fail(f"{name} is outside its finite range")
    return value


def number(value: Any, name: str, *, positive: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail(f"{name} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result < (0.0 if not positive else 1e-300):
        fail(f"{name} must be finite and {'positive' if positive else 'nonnegative'}")
    return result


def close(actual: float, expected: float) -> bool:
    return math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-9)


def validate_document(document: dict[str, Any], require_release: bool = False) -> None:
    if document.get("schema") != SCHEMA:
        fail("unexpected shared-Hub benchmark schema")
    if document.get("status") != "ok":
        fail("shared-Hub benchmark status is not ok")
    if require_release and document.get("build_type") != "Release":
        fail("shared-Hub benchmark is not a Release build")

    parameters = document.get("parameters")
    measurements = document.get("measurements")
    if not isinstance(parameters, dict) or not isinstance(measurements, dict):
        fail("parameters and measurements must be objects")

    connections = integer(parameters.get("connections"), "connections", positive=True)
    rounds = integer(
        parameters.get("round_trips_per_connection"),
        "round_trips_per_connection",
        positive=True,
    )
    payload = integer(parameters.get("payload_bytes"), "payload_bytes", positive=True)
    if connections < 2 or connections > 1024:
        fail("connections violate the reviewed bound")
    expected_round_trips = connections * rounds
    if expected_round_trips > 10_000_000:
        fail("total round trips violate the reviewed bound")

    completed = integer(
        measurements.get("completed_round_trips"),
        "completed_round_trips",
        positive=True,
    )
    if completed != expected_round_trips:
        fail("completed round trips do not match parameters")
    elapsed = number(measurements.get("elapsed_seconds"), "elapsed_seconds", positive=True)
    rate = number(
        measurements.get("round_trips_per_second"),
        "round_trips_per_second",
        positive=True,
    )
    throughput = number(
        measurements.get("throughput_mib_per_second"),
        "throughput_mib_per_second",
        positive=True,
    )
    if not close(rate, completed / elapsed):
        fail("round-trip rate is inconsistent")
    expected_throughput = 2.0 * completed * payload / (1024.0 * 1024.0 * elapsed)
    if not close(throughput, expected_throughput):
        fail("throughput is inconsistent")

    p50 = number(measurements.get("p50_latency_us"), "p50_latency_us")
    p99 = number(measurements.get("p99_latency_us"), "p99_latency_us")
    p999 = number(measurements.get("p999_latency_us"), "p999_latency_us")
    if not p50 <= p99 <= p999:
        fail("latency percentiles are not ordered")

    before = integer(measurements.get("working_set_before_bytes"), "working_set_before_bytes")
    active = integer(measurements.get("working_set_active_bytes"), "working_set_active_bytes")
    integer(measurements.get("working_set_after_bytes"), "working_set_after_bytes")
    delta = measurements.get("working_set_active_delta_bytes")
    if isinstance(delta, bool) or not isinstance(delta, int) or delta != active - before:
        fail("working-set active delta is inconsistent")
    per_connection = number(
        measurements.get("working_set_bytes_per_connection"),
        "working_set_bytes_per_connection",
    )
    expected_per_connection = max(delta, 0) / connections
    if not close(per_connection, expected_per_connection):
        fail("working-set bytes per connection are inconsistent")
    number(measurements.get("shutdown_milliseconds"), "shutdown_milliseconds")

    expected_bytes = completed * payload
    exact = {
        "connections_accepted": connections,
        "connections_retired": connections,
        "max_active_connections": connections,
        "send_admissions": completed,
        "bytes_sent": expected_bytes,
        "bytes_received": expected_bytes,
        "bytes_discarded": 0,
        "engine_rejections": 0,
        "cross_domain_fallbacks": 0,
        "active_operation_routes": 0,
        "pending_send_bytes": 0,
        "engine_active_operations": 0,
        "engine_ready_notices": 0,
        "engine_owned_bytes": 0,
    }
    for name, expected in exact.items():
        actual = integer(measurements.get(name), name)
        if actual != expected:
            fail(f"{name} does not match the converged contract")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Validate structured io_uring shared-Hub benchmark evidence"
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("--require-release", action="store_true")
    args = parser.parse_args()
    try:
        document = json.loads(args.input.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            fail("benchmark root must be an object")
        validate_document(document, require_release=args.require_release)
    except (OSError, json.JSONDecodeError, SharedHubBenchmarkValidationError) as error:
        raise SystemExit(f"shared-Hub benchmark validation failed: {error}") from error
    print(f"validated io_uring shared-Hub benchmark: {args.input}")


if __name__ == "__main__":
    main()
