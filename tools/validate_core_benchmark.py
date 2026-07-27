#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.core_benchmark.v2"
SCENARIOS = {"echo", "connections", "slow-client"}


class CoreBenchmarkValidationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CoreBenchmarkValidationError(message)


def non_negative_integer(value: Any, label: str) -> int:
    require(
        isinstance(value, int) and not isinstance(value, bool) and value >= 0,
        f"{label} must be a non-negative integer",
    )
    return value


def non_negative_number(value: Any, label: str, *, nullable: bool = False) -> float | None:
    if nullable and value is None:
        return None
    require(
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value >= 0,
        f"{label} must be a non-negative number" + (" or null" if nullable else ""),
    )
    return float(value)


def finite_number(value: Any, label: str, *, nullable: bool = False) -> float | None:
    if nullable and value is None:
        return None
    require(
        isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value),
        f"{label} must be a finite number" + (" or null" if nullable else ""),
    )
    return float(value)


def validate_document(
    document: Any,
    *,
    expected_platform: str | None = None,
    expected_backend: str | None = None,
    expected_build_type: str | None = None,
    expected_scenario: str | None = None,
    expected_connections: int | None = None,
    expected_slow_bytes: int | None = None,
    require_overload: bool = False,
    label: str = "Core benchmark document",
) -> dict[str, Any]:
    require(isinstance(document, dict), f"{label} must be a JSON object")
    require(document.get("schema") == SCHEMA, f"{label}: schema must be {SCHEMA}")
    require(document.get("status") == "ok", f"{label}: status must be ok")
    require(document.get("error") is None, f"{label}: successful result must have null error")

    scenario = document.get("scenario")
    require(scenario in SCENARIOS, f"{label}: unsupported scenario {scenario!r}")
    if expected_scenario is not None:
        require(scenario == expected_scenario, f"{label}: scenario mismatch")
    if expected_platform is not None:
        require(document.get("platform") == expected_platform, f"{label}: platform mismatch")
    if expected_backend is not None:
        require(document.get("backend") == expected_backend, f"{label}: backend mismatch")
    if expected_build_type is not None:
        require(document.get("build_type") == expected_build_type, f"{label}: build type mismatch")
    require(
        document.get("backpressure_policy") == "bounded_output_hysteresis",
        f"{label}: backpressure policy mismatch",
    )

    parameters = document.get("parameters")
    measurements = document.get("measurements")
    require(isinstance(parameters, dict), f"{label}: parameters must be an object")
    require(isinstance(measurements, dict), f"{label}: measurements must be an object")

    connections = non_negative_integer(parameters.get("connections"), f"{label}.connections")
    require(connections > 0, f"{label}: connections must be positive")
    slow_bytes = non_negative_integer(
        parameters.get("slow_bytes_per_connection"),
        f"{label}.slow_bytes_per_connection",
    )
    high_water = non_negative_integer(
        parameters.get("high_water_bytes"),
        f"{label}.high_water_bytes",
    )
    low_water = non_negative_integer(
        parameters.get("low_water_bytes"),
        f"{label}.low_water_bytes",
    )
    parameter_hard_limit = non_negative_integer(
        parameters.get("hard_limit_bytes"),
        f"{label}.hard_limit_bytes",
    )
    max_input_buffer = non_negative_integer(
        parameters.get("max_input_buffer_bytes"),
        f"{label}.max_input_buffer_bytes",
    )
    if expected_connections is not None:
        require(connections == expected_connections, f"{label}: connection count mismatch")
    if expected_slow_bytes is not None:
        require(slow_bytes == expected_slow_bytes, f"{label}: slow byte count mismatch")

    round_trips = non_negative_integer(measurements.get("round_trips"), f"{label}.round_trips")
    application_bytes = non_negative_integer(
        measurements.get("application_bytes"),
        f"{label}.application_bytes",
    )
    working_set_before = non_negative_integer(
        measurements.get("working_set_before_bytes"),
        f"{label}.working_set_before_bytes",
    )
    working_set_after = non_negative_integer(
        measurements.get("working_set_after_bytes"),
        f"{label}.working_set_after_bytes",
    )
    working_set_delta = measurements.get("working_set_delta_bytes")
    require(
        isinstance(working_set_delta, int) and not isinstance(working_set_delta, bool),
        f"{label}.working_set_delta_bytes must be an integer",
    )
    require(
        working_set_delta == working_set_after - working_set_before,
        f"{label}: working-set delta does not match before/after samples",
    )
    approximate_per_connection = finite_number(
        measurements.get("approx_bytes_per_connection"),
        f"{label}.approx_bytes_per_connection",
    )
    require(
        abs(approximate_per_connection - working_set_delta / connections) <= 0.001,
        f"{label}: per-connection working-set estimate is inconsistent",
    )
    non_negative_number(measurements.get("elapsed_seconds"), f"{label}.elapsed_seconds")
    throughput = non_negative_number(
        measurements.get("throughput_mib_per_second"),
        f"{label}.throughput_mib_per_second",
        nullable=True,
    )
    p50_latency = non_negative_number(
        measurements.get("p50_latency_us"),
        f"{label}.p50_latency_us",
        nullable=True,
    )
    p99_latency = non_negative_number(
        measurements.get("p99_latency_us"),
        f"{label}.p99_latency_us",
        nullable=True,
    )
    if scenario == "echo":
        messages = non_negative_integer(
            parameters.get("messages_per_connection"),
            f"{label}.messages_per_connection",
        )
        payload_bytes = non_negative_integer(
            parameters.get("payload_bytes"),
            f"{label}.payload_bytes",
        )
        require(
            round_trips == connections * messages,
            f"{label}: echo round-trip accounting mismatch",
        )
        require(
            application_bytes == round_trips * payload_bytes * 2,
            f"{label}: echo application-byte accounting mismatch",
        )
        require(
            throughput is not None and p50_latency is not None and p99_latency is not None,
            f"{label}: echo performance measurements must be present",
        )
        require(p50_latency <= p99_latency, f"{label}: P50 latency exceeds P99")
    else:
        require(round_trips == 0 and application_bytes == 0, f"{label}: non-echo traffic accounting")
        require(
            throughput is None and p50_latency is None and p99_latency is None,
            f"{label}: non-echo scenario reported echo performance measurements",
        )

    fields = {
        name: non_negative_integer(measurements.get(name), f"{label}.{name}")
        for name in (
            "requested_bytes",
            "accepted_bytes",
            "rejected_bytes",
            "accepted_sends",
            "rejected_sends",
            "overloaded_sends",
            "closed_sends",
            "owner_unavailable_sends",
            "output_hard_limit_bytes",
            "pending_output_peak_bytes",
            "read_pause_observations",
            "read_resume_observations",
            "high_water_callbacks",
        )
    }
    recovery = non_negative_number(
        measurements.get("backpressure_recovery_seconds"),
        f"{label}.backpressure_recovery_seconds",
        nullable=True,
    )
    require(fields["output_hard_limit_bytes"] > 0, f"{label}: output hard limit must be positive")
    require(
        0 < low_water < high_water < fields["output_hard_limit_bytes"],
        f"{label}: output watermarks must satisfy 0 < low < high < hard",
    )
    require(
        parameter_hard_limit == fields["output_hard_limit_bytes"],
        f"{label}: parameter and measurement hard limits differ",
    )
    require(max_input_buffer > 0, f"{label}: input buffer limit must be positive")

    accounting_fields = (
        "requested_bytes",
        "accepted_bytes",
        "rejected_bytes",
        "accepted_sends",
        "rejected_sends",
        "overloaded_sends",
        "closed_sends",
        "owner_unavailable_sends",
        "pending_output_peak_bytes",
        "read_pause_observations",
        "read_resume_observations",
        "high_water_callbacks",
    )
    if scenario != "slow-client":
        require(
            all(fields[name] == 0 for name in accounting_fields),
            f"{label}: non-slow scenario reported slow-client accounting",
        )
        require(recovery is None, f"{label}: non-slow scenario reported recovery duration")
        require(not require_overload, f"{label}: overload can only be required for slow-client")
        return document

    require(
        fields["requested_bytes"] == connections * slow_bytes,
        f"{label}: requested byte accounting mismatch",
    )
    require(
        fields["requested_bytes"] == fields["accepted_bytes"] + fields["rejected_bytes"],
        f"{label}: requested bytes do not equal accepted plus rejected",
    )
    require(
        fields["accepted_sends"] + fields["rejected_sends"] == connections,
        f"{label}: send decision count mismatch",
    )
    require(
        fields["overloaded_sends"] + fields["closed_sends"] +
        fields["owner_unavailable_sends"] == fields["rejected_sends"],
        f"{label}: rejection reason count mismatch",
    )
    require(fields["closed_sends"] == 0, f"{label}: benchmark connection closed before send")
    require(
        fields["owner_unavailable_sends"] == 0,
        f"{label}: benchmark connection owner was unavailable",
    )
    require(
        fields["accepted_bytes"] == fields["accepted_sends"] * slow_bytes,
        f"{label}: accepted byte accounting mismatch",
    )
    require(
        fields["rejected_bytes"] == fields["rejected_sends"] * slow_bytes,
        f"{label}: rejected byte accounting mismatch",
    )
    require(
        fields["accepted_sends"] == 0 or fields["rejected_sends"] == 0,
        f"{label}: mixed send decisions cannot be mapped to clients deterministically",
    )
    require(
        fields["pending_output_peak_bytes"] <= fields["output_hard_limit_bytes"],
        f"{label}: pending output peak exceeds hard limit",
    )
    require(
        fields["read_pause_observations"] <= fields["accepted_sends"],
        f"{label}: read pause count exceeds accepted sends",
    )
    require(
        fields["read_resume_observations"] <= fields["accepted_sends"],
        f"{label}: read resume count exceeds accepted sends",
    )
    if fields["accepted_sends"] > 0:
        require(recovery is not None, f"{label}: accepted output lacks recovery duration")
        if slow_bytes > high_water:
            require(
                fields["pending_output_peak_bytes"] > 0,
                f"{label}: accepted slow output did not report pending bytes",
            )
            require(
                fields["read_pause_observations"] == fields["accepted_sends"],
                f"{label}: each accepted slow output must observe one read pause",
            )
            require(
                fields["read_resume_observations"] == fields["accepted_sends"],
                f"{label}: each accepted slow output must observe one read resume",
            )
            require(
                fields["high_water_callbacks"] == fields["accepted_sends"],
                f"{label}: each accepted slow output must emit one high-water callback",
            )
    else:
        require(recovery is None, f"{label}: fully rejected output reported recovery duration")
        require(
            fields["pending_output_peak_bytes"] == 0,
            f"{label}: fully rejected output reserved pending bytes",
        )

    if require_overload:
        require(fields["rejected_sends"] > 0, f"{label}: overload was not observed")
        require(
            fields["overloaded_sends"] == fields["rejected_sends"],
            f"{label}: rejection was not entirely caused by output overload",
        )
    return document


def load_document(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        raise CoreBenchmarkValidationError(f"cannot read {path}: {error}") from error


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Core benchmark v2 JSON semantics")
    parser.add_argument("--input", type=Path, action="append", default=[])
    parser.add_argument("--input-dir", type=Path)
    parser.add_argument("--platform")
    parser.add_argument("--backend")
    parser.add_argument("--build-type")
    parser.add_argument("--scenario", choices=sorted(SCENARIOS))
    parser.add_argument("--expected-connections", type=int)
    parser.add_argument("--expected-slow-bytes", type=int)
    parser.add_argument("--require-overload", action="store_true")
    args = parser.parse_args()

    paths = list(args.input)
    if args.input_dir is not None:
        paths.extend(sorted(args.input_dir.glob("*.json")))
    if not paths:
        parser.error("at least one --input or a non-empty --input-dir is required")

    try:
        for path in paths:
            validate_document(
                load_document(path),
                expected_platform=args.platform,
                expected_backend=args.backend,
                expected_build_type=args.build_type,
                expected_scenario=args.scenario,
                expected_connections=args.expected_connections,
                expected_slow_bytes=args.expected_slow_bytes,
                require_overload=args.require_overload,
                label=str(path),
            )
    except CoreBenchmarkValidationError as error:
        print(f"Core benchmark validation error: {error}", file=sys.stderr)
        return 1
    print(f"validated {len(paths)} Core benchmark v2 artifact(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
