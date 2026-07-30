#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.core_benchmark.v2"
SCENARIOS = {"echo", "connections", "connection-churn", "slow-client"}
CLOSE_REASON_NAMES = {
    "peer_eof",
    "reset",
    "connect_timeout",
    "input_limit",
    "output_overload",
    "admission_policy",
    "graceful_shutdown",
    "forced_shutdown",
    "callback_failure",
    "internal_error",
}


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


def non_negative_integer_list(value: Any, label: str) -> list[int]:
    require(isinstance(value, list), f"{label} must be an array")
    return [
        non_negative_integer(item, f"{label}[{index}]")
        for index, item in enumerate(value)
    ]


def close_reason_counts(value: Any, label: str) -> dict[str, int]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(
        set(value) == CLOSE_REASON_NAMES,
        f"{label} must contain the exact close-reason key set",
    )
    return {
        name: non_negative_integer(value[name], f"{label}.{name}")
        for name in CLOSE_REASON_NAMES
    }


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
    require_zero_churn_failures: bool = False,
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
    settle_ms = non_negative_integer(
        parameters.get("settle_ms"),
        f"{label}.settle_ms",
    )
    event_loop_threads = non_negative_integer(
        parameters.get("event_loop_threads"),
        f"{label}.event_loop_threads",
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
    elapsed_seconds = non_negative_number(
        measurements.get("elapsed_seconds"),
        f"{label}.elapsed_seconds",
    )
    throughput = non_negative_number(
        measurements.get("throughput_mib_per_second"),
        f"{label}.throughput_mib_per_second",
        nullable=True,
    )
    messages_per_second = non_negative_number(
        measurements.get("messages_per_second"),
        f"{label}.messages_per_second",
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
    p999_latency = non_negative_number(
        measurements.get("p999_latency_us"),
        f"{label}.p999_latency_us",
        nullable=True,
    )
    connection_establish_seconds = non_negative_number(
        measurements.get("connection_establish_seconds"),
        f"{label}.connection_establish_seconds",
        nullable=True,
    )
    connection_establish_per_second = non_negative_number(
        measurements.get("connection_establish_per_second"),
        f"{label}.connection_establish_per_second",
        nullable=True,
    )
    idle_observation_seconds = non_negative_number(
        measurements.get("idle_observation_seconds"),
        f"{label}.idle_observation_seconds",
        nullable=True,
    )
    idle_process_cpu_seconds = non_negative_number(
        measurements.get("idle_process_cpu_seconds"),
        f"{label}.idle_process_cpu_seconds",
        nullable=True,
    )
    idle_process_cpu_percent = non_negative_number(
        measurements.get("idle_process_cpu_percent"),
        f"{label}.idle_process_cpu_percent",
        nullable=True,
    )
    connection_close_seconds = non_negative_number(
        measurements.get("connection_close_seconds"),
        f"{label}.connection_close_seconds",
        nullable=True,
    )
    server_stop_seconds = non_negative_number(
        measurements.get("server_stop_seconds"),
        f"{label}.server_stop_seconds",
        nullable=True,
    )
    server_stop_outcome = measurements.get("server_stop_outcome")
    require(
        isinstance(server_stop_outcome, str),
        f"{label}.server_stop_outcome must be a string",
    )
    server_stop_initial_connections = non_negative_integer(
        measurements.get("server_stop_initial_connections"),
        f"{label}.server_stop_initial_connections",
    )
    server_stop_forced_connections = non_negative_integer(
        measurements.get("server_stop_forced_connections"),
        f"{label}.server_stop_forced_connections",
    )
    churn_attempted = non_negative_integer(
        measurements.get("churn_attempted_connections"),
        f"{label}.churn_attempted_connections",
    )
    churn_accepted = non_negative_integer(
        measurements.get("churn_accepted_connections"),
        f"{label}.churn_accepted_connections",
    )
    churn_connect_failures = non_negative_integer(
        measurements.get("churn_connect_failures"),
        f"{label}.churn_connect_failures",
    )
    churn_closed = non_negative_integer(
        measurements.get("churn_closed_connections"),
        f"{label}.churn_closed_connections",
    )
    churn_batches = non_negative_integer(
        measurements.get("churn_batches"),
        f"{label}.churn_batches",
    )
    churn_elapsed = non_negative_number(
        measurements.get("churn_elapsed_seconds"),
        f"{label}.churn_elapsed_seconds",
        nullable=True,
    )
    churn_attempt_rate = non_negative_number(
        measurements.get("churn_attempts_per_second"),
        f"{label}.churn_attempts_per_second",
        nullable=True,
    )
    churn_accept_rate = non_negative_number(
        measurements.get("churn_accepts_per_second"),
        f"{label}.churn_accepts_per_second",
        nullable=True,
    )
    churn_close_rate = non_negative_number(
        measurements.get("churn_closes_per_second"),
        f"{label}.churn_closes_per_second",
        nullable=True,
    )
    churn_worker_accept_counts = non_negative_integer_list(
        measurements.get("churn_worker_accept_counts"),
        f"{label}.churn_worker_accept_counts",
    )
    churn_worker_skew = non_negative_number(
        measurements.get("churn_worker_skew_ratio"),
        f"{label}.churn_worker_skew_ratio",
        nullable=True,
    )
    churn_phase_metrics = {
        name: non_negative_number(
            measurements.get(name),
            f"{label}.{name}",
            nullable=True,
        )
        for name in (
            "churn_connect_p99_us",
            "churn_connect_max_us",
            "churn_accept_p99_us",
            "churn_accept_max_us",
            "churn_close_p99_us",
            "churn_close_max_us",
            "churn_schedule_lag_p99_us",
            "churn_schedule_lag_max_us",
        )
    }
    churn_close_reasons = close_reason_counts(
        measurements.get("churn_close_reason_counts"),
        f"{label}.churn_close_reason_counts",
    )
    require(
        connection_close_seconds is not None,
        f"{label}: successful run must report connection-close convergence",
    )
    require(
        server_stop_seconds is not None,
        f"{label}: successful run must report server-stop duration",
    )
    require(server_stop_outcome == "drained", f"{label}: successful stop must drain")
    require(
        server_stop_initial_connections == 0,
        f"{label}: server stop began before the connection map was empty",
    )
    require(
        server_stop_forced_connections == 0,
        f"{label}: successful server stop forced connections",
    )
    connection_only = (
        connection_establish_seconds,
        connection_establish_per_second,
        idle_observation_seconds,
        idle_process_cpu_seconds,
        idle_process_cpu_percent,
    )
    if scenario == "connections":
        require(
            all(value is not None for value in connection_only),
            f"{label}: connections scenario must report establish and idle CPU metrics",
        )
        require(
            connection_establish_seconds > 0.0,
            f"{label}: connection establishment duration must be positive",
        )
        require(
            idle_observation_seconds > 0.0,
            f"{label}: idle observation duration must be positive",
        )
        expected_rate = connections / connection_establish_seconds
        require(
            math.isclose(
                connection_establish_per_second,
                expected_rate,
                rel_tol=0.001,
                abs_tol=0.001,
            ),
            f"{label}: connection establishment rate is inconsistent",
        )
        require(
            idle_observation_seconds + 0.001 >= settle_ms / 1000.0,
            f"{label}: idle observation is shorter than configured settle time",
        )
        expected_cpu_percent = (
            idle_process_cpu_seconds * 100.0 / idle_observation_seconds
        )
        require(
            math.isclose(
                idle_process_cpu_percent,
                expected_cpu_percent,
                rel_tol=0.001,
                abs_tol=0.001,
            ),
            f"{label}: idle CPU percent is inconsistent",
        )
        require(
            elapsed_seconds + 0.001 >=
            connection_establish_seconds + idle_observation_seconds,
            f"{label}: legacy elapsed time omits an establishment or idle phase",
        )
    else:
        require(
            all(value is None for value in connection_only),
            f"{label}: non-connections scenario reported idle-capacity metrics",
        )

    churn_rates = (
        churn_elapsed,
        churn_attempt_rate,
        churn_accept_rate,
        churn_close_rate,
        churn_worker_skew,
    )
    if scenario == "connection-churn":
        churn_target = non_negative_integer(
            parameters.get("churn_target_per_second"),
            f"{label}.churn_target_per_second",
        )
        churn_duration_ms = non_negative_integer(
            parameters.get("churn_duration_ms"),
            f"{label}.churn_duration_ms",
        )
        churn_connect_timeout_ms = non_negative_integer(
            parameters.get("churn_connect_timeout_ms"),
            f"{label}.churn_connect_timeout_ms",
        )
        timeout_ms = non_negative_integer(
            parameters.get("timeout_ms"),
            f"{label}.timeout_ms",
        )
        require(churn_target > 0, f"{label}: churn target must be positive")
        require(churn_duration_ms > 0, f"{label}: churn duration must be positive")
        require(
            churn_connect_timeout_ms > 0,
            f"{label}: churn connect timeout must be positive",
        )
        require(
            churn_duration_ms <= timeout_ms
            and churn_connect_timeout_ms <= timeout_ms,
            f"{label}: churn deadlines exceed the overall timeout",
        )
        expected_attempts = churn_target * churn_duration_ms // 1000
        require(expected_attempts > 0, f"{label}: churn parameters produce zero attempts")
        require(
            churn_attempted == expected_attempts,
            f"{label}: churn attempt count does not match target and duration",
        )
        require(
            churn_batches == (expected_attempts + connections - 1) // connections,
            f"{label}: churn batch count is inconsistent",
        )
        require(
            churn_attempted == churn_accepted + churn_connect_failures,
            f"{label}: churn attempts do not equal accepted plus connect failures",
        )
        require(
            churn_accepted == churn_closed,
            f"{label}: churn accepted connections do not equal closed connections",
        )
        require(
            sum(churn_close_reasons.values()) == churn_closed,
            f"{label}: churn close reasons do not sum to closed connections",
        )
        require(churn_accepted > 0, f"{label}: churn accepted no connections")
        require(
            all(value is not None for value in churn_rates),
            f"{label}: churn rate and worker-skew metrics must be present",
        )
        require(
            all(value is not None for value in churn_phase_metrics.values()),
            f"{label}: churn phase-latency metrics must be present",
        )
        require(churn_elapsed > 0.0, f"{label}: churn elapsed time must be positive")
        require(
            math.isclose(
                elapsed_seconds,
                churn_elapsed,
                rel_tol=0.001,
                abs_tol=0.001,
            ),
            f"{label}: common and churn elapsed times differ",
        )
        require(
            churn_elapsed + 0.001 >= churn_duration_ms / 1000.0,
            f"{label}: churn elapsed time is shorter than the paced duration",
        )
        for actual, count, rate_name in (
            (churn_attempt_rate, churn_attempted, "attempt"),
            (churn_accept_rate, churn_accepted, "accept"),
            (churn_close_rate, churn_closed, "close"),
        ):
            require(
                math.isclose(
                    actual,
                    count / churn_elapsed,
                    rel_tol=0.001,
                    abs_tol=0.001,
                ),
                f"{label}: churn {rate_name} rate is inconsistent",
            )
        expected_worker_count = max(1, event_loop_threads)
        require(
            len(churn_worker_accept_counts) == expected_worker_count,
            f"{label}: churn worker count does not match configured EventLoops",
        )
        require(
            churn_worker_accept_counts == sorted(churn_worker_accept_counts),
            f"{label}: churn worker accept counts must use stable sorted order",
        )
        require(
            sum(churn_worker_accept_counts) == churn_accepted,
            f"{label}: churn worker accepts do not sum to accepted connections",
        )
        mean_worker_accepts = churn_accepted / expected_worker_count
        expected_skew = (
            max(churn_worker_accept_counts) - min(churn_worker_accept_counts)
        ) / mean_worker_accepts
        require(
            math.isclose(
                churn_worker_skew,
                expected_skew,
                rel_tol=0.001,
                abs_tol=0.001,
            ),
            f"{label}: churn worker skew is inconsistent",
        )
        for phase in ("connect", "accept", "close", "schedule_lag"):
            p99 = churn_phase_metrics[f"churn_{phase}_p99_us"]
            maximum = churn_phase_metrics[f"churn_{phase}_max_us"]
            require(
                p99 <= maximum,
                f"{label}: churn {phase} P99 exceeds maximum",
            )
            require(
                maximum <= churn_elapsed * 1000000.0 + 1.0,
                f"{label}: churn {phase} maximum exceeds total elapsed time",
            )
        if require_zero_churn_failures:
            require(
                churn_connect_failures == 0,
                f"{label}: churn connect failures are not allowed",
            )
    else:
        require(
            churn_attempted == 0
            and churn_accepted == 0
            and churn_connect_failures == 0
            and churn_closed == 0
            and churn_batches == 0
            and all(value is None for value in churn_rates)
            and all(value is None for value in churn_phase_metrics.values())
            and churn_worker_accept_counts == []
            and all(value == 0 for value in churn_close_reasons.values()),
            f"{label}: non-churn scenario reported churn measurements",
        )
        require(
            not require_zero_churn_failures,
            f"{label}: zero churn failures can only be required for connection-churn",
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
            all(
                value is not None
                for value in (
                    throughput,
                    messages_per_second,
                    p50_latency,
                    p99_latency,
                    p999_latency,
                )
            ),
            f"{label}: echo performance measurements must be present",
        )
        require(elapsed_seconds > 0.0, f"{label}: echo elapsed time must be positive")
        require(
            math.isclose(
                messages_per_second,
                round_trips / elapsed_seconds,
                rel_tol=0.001,
                abs_tol=0.001,
            ),
            f"{label}: echo message rate is inconsistent",
        )
        require(
            math.isclose(
                throughput,
                application_bytes / (1024.0 * 1024.0) / elapsed_seconds,
                rel_tol=0.001,
                abs_tol=0.001,
            ),
            f"{label}: echo throughput is inconsistent",
        )
        require(
            p50_latency <= p99_latency <= p999_latency,
            f"{label}: echo latency percentiles are not ordered",
        )
    else:
        require(round_trips == 0 and application_bytes == 0, f"{label}: non-echo traffic accounting")
        require(
            all(
                value is None
                for value in (
                    throughput,
                    messages_per_second,
                    p50_latency,
                    p99_latency,
                    p999_latency,
                )
            ),
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
    parser.add_argument("--require-zero-churn-failures", action="store_true")
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
                require_zero_churn_failures=args.require_zero_churn_failures,
                label=str(path),
            )
    except CoreBenchmarkValidationError as error:
        print(f"Core benchmark validation error: {error}", file=sys.stderr)
        return 1
    print(f"validated {len(paths)} Core benchmark v2 artifact(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
