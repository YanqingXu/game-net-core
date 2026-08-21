#!/usr/bin/env python3
"""Validate one fixed IOE-X10 listener comparison sample."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.ioe_x10_listener_comparison.v1"
PARAMETERS = {
    "active_routes": 256,
    "max_pending_accepts": 32,
    "hub_route_limit": 256,
    "churn_waves": 4,
    "replacements_per_wave": 64,
    "round_trips_per_route_per_wave": 100,
    "payload_bytes": 64,
}
EXPECTED_CONNECTS = 516
EXPECTED_ADMITTED = 512
EXPECTED_ROUND_TRIPS = 128000
EXPECTED_REJECTIONS = 4


class ListenerComparisonValidationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ListenerComparisonValidationError(message)


def finite_number(value: Any, label: str, *, minimum: float = 0.0) -> float:
    require(
        isinstance(value, (int, float)) and not isinstance(value, bool),
        f"{label} must be numeric",
    )
    number = float(value)
    require(math.isfinite(number) and number >= minimum, f"{label} is invalid")
    return number


def exact_integer(value: Any, expected: int, label: str) -> None:
    require(
        isinstance(value, int) and not isinstance(value, bool) and value == expected,
        f"{label} must be {expected}",
    )


def validate_document(
    document: dict[str, Any],
    *,
    expected_backend: str | None = None,
    require_release: bool = True,
) -> dict[str, Any]:
    require(document.get("schema") == SCHEMA, "unexpected IOE-X10 schema")
    require(document.get("status") == "ok", "sample status must be ok")
    backend = document.get("backend")
    require(backend in {"epoll", "io_uring"}, "unsupported backend")
    if expected_backend is not None:
        require(backend == expected_backend, "sample backend/order mismatch")
    if require_release:
        require(document.get("build_type") == "Release", "sample must be Release")
    require(document.get("parameters") == PARAMETERS, "fixed protocol drift")

    measurements = document.get("measurements")
    require(isinstance(measurements, dict), "measurements must be an object")
    exact_integer(measurements.get("connect_completions"), EXPECTED_CONNECTS, "connects")
    exact_integer(
        measurements.get("connections_admitted"), EXPECTED_ADMITTED, "admitted"
    )
    exact_integer(
        measurements.get("echo_completions"), EXPECTED_ROUND_TRIPS, "echoes"
    )
    exact_integer(
        measurements.get("close_completions"), EXPECTED_ADMITTED, "closes"
    )
    for name in (
        "connect_completion_rate",
        "echo_completion_rate",
        "close_completion_rate",
    ):
        require(finite_number(measurements.get(name), name) == 1.0, f"{name} must be 1")
    for name in (
        "connects_per_second",
        "round_trips_per_second",
        "throughput_mib_per_second",
        "p50_latency_us",
        "p99_latency_us",
        "p999_latency_us",
        "rss_before_bytes",
        "rss_high_water_bytes",
        "rss_after_bytes",
        "max_capacity_recovery_milliseconds",
        "listener_shutdown_milliseconds",
        "server_shutdown_milliseconds",
    ):
        finite_number(measurements.get(name), name)
    require(measurements["connects_per_second"] > 0, "connect throughput must be positive")
    require(measurements["round_trips_per_second"] > 0, "echo throughput must be positive")
    require(measurements["throughput_mib_per_second"] > 0, "byte throughput must be positive")
    require(
        measurements["p50_latency_us"] <= measurements["p99_latency_us"]
        <= measurements["p999_latency_us"],
        "latency percentiles are unordered",
    )

    fd_before = measurements.get("fd_baseline")
    fd_peak = measurements.get("fd_high_water")
    fd_after = measurements.get("fd_final")
    require(
        all(isinstance(value, int) and not isinstance(value, bool) for value in (fd_before, fd_peak, fd_after)),
        "fd observations must be integers",
    )
    require(fd_peak >= fd_before and fd_peak >= fd_after, "fd high water is invalid")
    require(fd_after == fd_before, "fd baseline/final mismatch")
    exact_integer(measurements.get("fd_residue"), 0, "fd residue")
    exact_integer(measurements.get("max_active_routes"), 256, "route high water")
    exact_integer(
        measurements.get("capacity_rejections"), EXPECTED_REJECTIONS, "capacity rejects"
    )
    require(measurements.get("capacity_recovered") is True, "capacity did not recover")
    require(measurements.get("listener_closed") is True, "listener did not close")
    exact_integer(measurements.get("final_active_routes"), 0, "final routes")
    exact_integer(measurements.get("final_pending_send_bytes"), 0, "final pending bytes")
    require(
        isinstance(measurements.get("max_pending_send_bytes"), int)
        and measurements["max_pending_send_bytes"] >= 0,
        "pending-send high water is invalid",
    )
    require(
        measurements["rss_high_water_bytes"] >= measurements["rss_before_bytes"]
        and measurements["rss_high_water_bytes"] >= measurements["rss_after_bytes"],
        "RSS high water is invalid",
    )

    completion_fields = (
        "max_active_accepts",
        "max_active_recvs",
        "max_active_sends",
        "max_active_operations",
        "max_engine_owned_bytes",
        "sq_rejections",
        "final_active_accepts",
        "final_active_operations",
        "final_ready_notices",
        "final_engine_owned_bytes",
    )
    if backend == "epoll":
        for name in completion_fields:
            require(measurements.get(name) is None, f"epoll must not synthesize {name}")
    else:
        exact_integer(measurements.get("max_active_accepts"), 32, "Accept high water")
        exact_integer(measurements.get("max_active_recvs"), 256, "Recv high water")
        require(
            isinstance(measurements.get("max_active_sends"), int)
            and 0 < measurements["max_active_sends"] <= 256,
            "Send high water is invalid",
        )
        require(
            isinstance(measurements.get("max_active_operations"), int)
            and 288 <= measurements["max_active_operations"] <= 1024,
            "operation high water is invalid",
        )
        require(
            isinstance(measurements.get("max_engine_owned_bytes"), int)
            and 0 < measurements["max_engine_owned_bytes"] <= 2 * 1024 * 1024,
            "Engine-owned-byte high water is invalid",
        )
        exact_integer(measurements.get("sq_rejections"), 0, "SQ rejects")
        for name in (
            "final_active_accepts",
            "final_active_operations",
            "final_ready_notices",
            "final_engine_owned_bytes",
        ):
            exact_integer(measurements.get(name), 0, name)
    return document


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("sample", type=Path)
    parser.add_argument("--backend", choices=("epoll", "io_uring"))
    args = parser.parse_args()
    document = json.loads(args.sample.read_text(encoding="utf-8"))
    validate_document(document, expected_backend=args.backend)
    print(f"validated {args.sample}")


if __name__ == "__main__":
    main()
