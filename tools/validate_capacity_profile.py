#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gamenet.capacity_profile.v1"
SCENARIO = "slow-broadcast-recovery"
REASONS = {
    "none",
    "offline_session",
    "duplicate_endpoint",
    "fanout_hard_limit",
    "byte_hard_limit",
    "low_priority_soft_limit",
    "dispatch_task_byte_limit",
    "endpoint_closed",
    "endpoint_overloaded",
    "owner_unavailable",
    "owner_shutdown",
    "dispatch_queue_full",
    "owner_outstanding_task_limit",
    "owner_outstanding_byte_limit",
    "global_outstanding_byte_limit",
    "invalid_plan",
    "send_rejected",
}
CHECKS = {
    "pending_within_limit",
    "broadcast_within_limit",
    "terminal_accounted",
    "client_delivery_accounted",
    "rejection_attributed",
    "overload_observed",
    "recovery_stable",
    "recovery_retained_within_target",
    "fixed_storage_coherent",
    "teardown_released",
    "passed",
}


class CapacityProfileValidationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CapacityProfileValidationError(message)


def object_field(parent: dict[str, Any], name: str, label: str) -> dict[str, Any]:
    value = parent.get(name)
    require(isinstance(value, dict), f"{label}.{name} must be an object")
    return value


def non_negative_integer(value: Any, label: str) -> int:
    require(
        isinstance(value, int) and not isinstance(value, bool) and value >= 0,
        f"{label} must be a non-negative integer",
    )
    return value


def positive_integer(value: Any, label: str) -> int:
    value = non_negative_integer(value, label)
    require(value > 0, f"{label} must be positive")
    return value


def non_negative_number(value: Any, label: str) -> float:
    require(
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value >= 0,
        f"{label} must be a non-negative finite number",
    )
    return float(value)


def integer(value: Any, label: str) -> int:
    require(
        isinstance(value, int) and not isinstance(value, bool),
        f"{label} must be an integer",
    )
    return value


def validate_fixed_storage(value: Any, label: str, *, require_zero: bool = False) -> None:
    require(isinstance(value, dict), f"{label} must be an object")
    fields = {
        name: non_negative_integer(value.get(name), f"{label}.{name}")
        for name in (
            "shared_read_pool_bytes",
            "shared_read_slab_bytes",
            "accept_ex_fixed_pool_bytes",
            "iocp_completion_batch_bytes",
            "connection_local_read_bytes",
            "total_retained_bytes",
            "peak_total_retained_bytes",
            "accept_ex_slot_limit_per_acceptor",
            "iocp_completion_batch_entries_per_loop",
            "connection_local_read_chunk_limit_bytes",
        )
    }
    require(fields["shared_read_pool_bytes"] == 0, f"{label}: shared read pool must be zero")
    require(fields["shared_read_slab_bytes"] == 0, f"{label}: shared read slab must be zero")
    require(
        fields["total_retained_bytes"]
        == fields["accept_ex_fixed_pool_bytes"]
        + fields["iocp_completion_batch_bytes"]
        + fields["connection_local_read_bytes"],
        f"{label}: fixed-storage total is inconsistent",
    )
    require(
        fields["total_retained_bytes"] <= fields["peak_total_retained_bytes"],
        f"{label}: fixed-storage current exceeds peak",
    )
    if require_zero:
        require(
            fields["total_retained_bytes"] == 0
            and fields["accept_ex_fixed_pool_bytes"] == 0
            and fields["iocp_completion_batch_bytes"] == 0
            and fields["connection_local_read_bytes"] == 0,
            f"{label}: teardown retained fixed storage",
        )


def validate_retention(
    value: Any,
    label: str,
    *,
    connections: int,
    buffer_target: int,
    read_chunk_limit: int | None = None,
    require_recovered_target: bool = False,
) -> None:
    require(isinstance(value, dict), f"{label} must be an object")
    fields = {
        name: non_negative_integer(value.get(name), f"{label}.{name}")
        for name in (
            "input_buffer_bytes",
            "output_buffer_bytes",
            "transport_read_storage_bytes",
            "total_connection_bytes",
            "peak_input_buffer_bytes",
            "peak_output_buffer_bytes",
            "peak_transport_read_storage_bytes",
            "input_trim_count",
            "output_trim_count",
        )
    }
    require(
        fields["total_connection_bytes"]
        == fields["input_buffer_bytes"]
        + fields["output_buffer_bytes"]
        + fields["transport_read_storage_bytes"],
        f"{label}: connection-retention total is inconsistent",
    )
    require(
        fields["input_buffer_bytes"] <= fields["peak_input_buffer_bytes"]
        and fields["output_buffer_bytes"] <= fields["peak_output_buffer_bytes"]
        and fields["transport_read_storage_bytes"]
        <= fields["peak_transport_read_storage_bytes"],
        f"{label}: retained current exceeds a component peak",
    )
    if require_recovered_target:
        require(
            fields["input_buffer_bytes"] + fields["output_buffer_bytes"]
            <= connections * 2 * buffer_target,
            f"{label}: recovered Buffer retention exceeds configured targets",
        )
        require(read_chunk_limit is not None, f"{label}: missing read chunk limit")
        require(
            fields["transport_read_storage_bytes"] <= connections * read_chunk_limit,
            f"{label}: connection-local read storage exceeds the per-connection limit",
        )


def validate_document(
    document: Any,
    *,
    expected_platform: str | None = None,
    expected_backend: str | None = None,
    expected_build_type: str | None = None,
    expected_connections: int | None = None,
    label: str = "capacity profile document",
) -> dict[str, Any]:
    require(isinstance(document, dict), f"{label} must be a JSON object")
    require(document.get("schema") == SCHEMA, f"{label}: schema must be {SCHEMA}")
    require(document.get("status") == "ok", f"{label}: status must be ok")
    require(document.get("error") is None, f"{label}: successful result must have null error")
    require(document.get("scenario") == SCENARIO, f"{label}: unsupported scenario")
    if expected_platform is not None:
        require(document.get("platform") == expected_platform, f"{label}: platform mismatch")
    if expected_backend is not None:
        require(document.get("backend") == expected_backend, f"{label}: backend mismatch")
    if expected_build_type is not None:
        require(document.get("build_type") == expected_build_type, f"{label}: build type mismatch")

    parameters = object_field(document, "parameters", label)
    limits = object_field(document, "limits", label)
    terminal = object_field(document, "terminal", label)
    pressure = object_field(document, "pressure", label)
    recovery = object_field(document, "recovery", label)
    process = object_field(document, "process", label)
    checks = object_field(document, "checks", label)

    connections = positive_integer(parameters.get("connections"), f"{label}.connections")
    threads = positive_integer(parameters.get("threads"), f"{label}.threads")
    messages = positive_integer(parameters.get("messages"), f"{label}.messages")
    payload_bytes = positive_integer(
        parameters.get("payload_bytes"), f"{label}.payload_bytes"
    )
    non_negative_integer(parameters.get("pressure_settle_ms"), f"{label}.pressure_settle_ms")
    recovery_stable_ms = positive_integer(
        parameters.get("recovery_stable_ms"), f"{label}.recovery_stable_ms"
    )
    positive_integer(parameters.get("timeout_ms"), f"{label}.timeout_ms")
    positive_integer(parameters.get("iocp_accept_depth"), f"{label}.iocp_accept_depth")
    if expected_connections is not None:
        require(connections == expected_connections, f"{label}: connection count mismatch")

    low_water = positive_integer(
        limits.get("connection_low_water_bytes"), f"{label}.connection_low_water_bytes"
    )
    high_water = positive_integer(
        limits.get("connection_high_water_bytes"), f"{label}.connection_high_water_bytes"
    )
    hard_limit = positive_integer(
        limits.get("connection_hard_limit_bytes"), f"{label}.connection_hard_limit_bytes"
    )
    aggregate_limit = positive_integer(
        limits.get("aggregate_pending_hard_limit_bytes"),
        f"{label}.aggregate_pending_hard_limit_bytes",
    )
    broadcast_limit = positive_integer(
        limits.get("broadcast_global_outstanding_limit_bytes"),
        f"{label}.broadcast_global_outstanding_limit_bytes",
    )
    recovery_threshold = non_negative_integer(
        limits.get("recovery_pending_threshold_bytes"),
        f"{label}.recovery_pending_threshold_bytes",
    )
    buffer_target = positive_integer(
        limits.get("buffer_max_retained_capacity_bytes"),
        f"{label}.buffer_max_retained_capacity_bytes",
    )
    require(low_water <= high_water <= hard_limit, f"{label}: invalid connection watermarks")
    require(recovery_threshold <= low_water, f"{label}: recovery threshold exceeds low water")
    require(
        aggregate_limit == connections * hard_limit,
        f"{label}: aggregate pending limit does not equal connections times hard limit",
    )

    scheduled = non_negative_integer(
        terminal.get("scheduled_endpoints"), f"{label}.terminal.scheduled_endpoints"
    )
    accepted = non_negative_integer(
        terminal.get("accepted_endpoints"), f"{label}.terminal.accepted_endpoints"
    )
    dropped = non_negative_integer(
        terminal.get("dropped_endpoints"), f"{label}.terminal.dropped_endpoints"
    )
    require(terminal.get("complete") is True, f"{label}: terminal progress is incomplete")
    require(
        scheduled == connections * messages,
        f"{label}: scheduled endpoint count mismatch",
    )
    require(
        accepted + dropped == connections * messages,
        f"{label}: terminal accepted/drop accounting mismatch",
    )
    reasons = object_field(terminal, "reasons", f"{label}.terminal")
    require(set(reasons) == REASONS, f"{label}: terminal reason set mismatch")
    reason_counts = {
        name: non_negative_integer(value, f"{label}.terminal.reasons.{name}")
        for name, value in reasons.items()
    }
    require(
        sum(reason_counts.values()) == dropped,
        f"{label}: typed reason counts do not equal dropped endpoints",
    )
    require(
        reason_counts["endpoint_overloaded"] > 0,
        f"{label}: profile did not observe endpoint overload",
    )
    require(
        sum(value for name, value in reason_counts.items() if name != "endpoint_overloaded")
        == 0,
        f"{label}: profile contains an unattributed non-overload terminal reason",
    )

    rejections = object_field(terminal, "tcp_rejections", f"{label}.terminal")
    rejection_fields = {
        name: non_negative_integer(value, f"{label}.terminal.tcp_rejections.{name}")
        for name, value in rejections.items()
    }
    require(
        set(rejection_fields) == {"connection", "loop", "server", "global", "total"},
        f"{label}: TCP rejection scope set mismatch",
    )
    require(
        rejection_fields["total"]
        == rejection_fields["connection"]
        + rejection_fields["loop"]
        + rejection_fields["server"]
        + rejection_fields["global"],
        f"{label}: TCP rejection scope total is inconsistent",
    )
    require(
        rejection_fields["total"] == reason_counts["endpoint_overloaded"],
        f"{label}: EndpointOverloaded does not reconcile with TCP rejection scopes",
    )

    non_negative_number(pressure.get("elapsed_ms"), f"{label}.pressure.elapsed_ms")
    pressure_current = non_negative_integer(
        pressure.get("pending_current_bytes"), f"{label}.pressure.pending_current_bytes"
    )
    pressure_peak = non_negative_integer(
        pressure.get("pending_peak_bytes"), f"{label}.pressure.pending_peak_bytes"
    )
    non_negative_integer(
        pressure.get("overloaded_connections"), f"{label}.pressure.overloaded_connections"
    )
    pressure_broadcast_tasks = non_negative_integer(
        pressure.get("broadcast_outstanding_tasks"),
        f"{label}.pressure.broadcast_outstanding_tasks",
    )
    pressure_broadcast_bytes = non_negative_integer(
        pressure.get("broadcast_outstanding_bytes"),
        f"{label}.pressure.broadcast_outstanding_bytes",
    )
    pressure_broadcast_peak_tasks = non_negative_integer(
        pressure.get("broadcast_peak_tasks"), f"{label}.pressure.broadcast_peak_tasks"
    )
    pressure_broadcast_peak_bytes = non_negative_integer(
        pressure.get("broadcast_peak_bytes"), f"{label}.pressure.broadcast_peak_bytes"
    )
    non_negative_integer(pressure.get("working_set_bytes"), f"{label}.pressure.working_set_bytes")
    require(
        pressure_current <= pressure_peak <= aggregate_limit,
        f"{label}: pressure pending bytes exceed the aggregate hard limit",
    )
    require(
        pressure_broadcast_tasks <= pressure_broadcast_peak_tasks
        and pressure_broadcast_bytes <= pressure_broadcast_peak_bytes <= broadcast_limit,
        f"{label}: Broadcast outstanding bytes/tasks exceed configured peaks or limit",
    )
    validate_retention(
        pressure.get("connection_retention"),
        f"{label}.pressure.connection_retention",
        connections=connections,
        buffer_target=buffer_target,
    )
    validate_fixed_storage(pressure.get("fixed_storage"), f"{label}.pressure.fixed_storage")

    recovery_elapsed = non_negative_number(
        recovery.get("elapsed_ms"), f"{label}.recovery.elapsed_ms"
    )
    stable_window = positive_integer(
        recovery.get("stable_window_ms"), f"{label}.recovery.stable_window_ms"
    )
    require(stable_window == recovery_stable_ms, f"{label}: recovery stable window mismatch")
    require(recovery_elapsed >= stable_window, f"{label}: recovery did not sustain its stable window")
    recovery_pending = non_negative_integer(
        recovery.get("pending_current_bytes"), f"{label}.recovery.pending_current_bytes"
    )
    recovery_peak = non_negative_integer(
        recovery.get("pending_peak_bytes"), f"{label}.recovery.pending_peak_bytes"
    )
    recovery_tasks = non_negative_integer(
        recovery.get("broadcast_outstanding_tasks"),
        f"{label}.recovery.broadcast_outstanding_tasks",
    )
    recovery_bytes = non_negative_integer(
        recovery.get("broadcast_outstanding_bytes"),
        f"{label}.recovery.broadcast_outstanding_bytes",
    )
    non_negative_integer(recovery.get("working_set_bytes"), f"{label}.recovery.working_set_bytes")
    integer(
        recovery.get("working_set_delta_from_baseline_bytes"),
        f"{label}.recovery.working_set_delta_from_baseline_bytes",
    )
    require(
        recovery_pending <= recovery_threshold and recovery_peak == pressure_peak,
        f"{label}: recovery pending bytes are outside the stable threshold or peak changed",
    )
    require(recovery_tasks == 0 and recovery_bytes == 0, f"{label}: Broadcast did not converge")
    recovery_fixed = object_field(recovery, "fixed_storage", f"{label}.recovery")
    validate_fixed_storage(recovery_fixed, f"{label}.recovery.fixed_storage")
    read_chunk_limit = non_negative_integer(
        recovery_fixed.get("connection_local_read_chunk_limit_bytes"),
        f"{label}.recovery.fixed_storage.connection_local_read_chunk_limit_bytes",
    )
    validate_retention(
        recovery.get("connection_retention"),
        f"{label}.recovery.connection_retention",
        connections=connections,
        buffer_target=buffer_target,
        read_chunk_limit=read_chunk_limit,
        require_recovered_target=True,
    )

    working_set_before = non_negative_integer(
        process.get("working_set_before_bytes"), f"{label}.process.working_set_before_bytes"
    )
    working_set_after = non_negative_integer(
        process.get("working_set_after_bytes"), f"{label}.process.working_set_after_bytes"
    )
    working_set_peak = non_negative_integer(
        process.get("working_set_peak_bytes"), f"{label}.process.working_set_peak_bytes"
    )
    client_received = non_negative_integer(
        process.get("client_received_bytes"), f"{label}.process.client_received_bytes"
    )
    require(
        client_received == accepted * payload_bytes,
        f"{label}: client delivery bytes do not equal accepted endpoint bytes",
    )
    require(
        working_set_peak
        >= max(
            working_set_before,
            working_set_after,
            non_negative_integer(
                pressure.get("working_set_bytes"), f"{label}.pressure.working_set_bytes"
            ),
            non_negative_integer(
                recovery.get("working_set_bytes"), f"{label}.recovery.working_set_bytes"
            ),
        ),
        f"{label}: working-set peak is below an observed sample",
    )
    require(
        integer(
            recovery.get("working_set_delta_from_baseline_bytes"),
            f"{label}.recovery.working_set_delta_from_baseline_bytes",
        )
        == non_negative_integer(
            recovery.get("working_set_bytes"), f"{label}.recovery.working_set_bytes"
        )
        - working_set_before,
        f"{label}: recovery working-set delta is inconsistent",
    )
    validate_fixed_storage(
        process.get("fixed_storage_baseline"), f"{label}.process.fixed_storage_baseline"
    )
    validate_fixed_storage(
        process.get("fixed_storage_after_teardown"),
        f"{label}.process.fixed_storage_after_teardown",
        require_zero=True,
    )

    require(set(checks) == CHECKS, f"{label}: check set mismatch")
    require(
        all(checks.get(name) is True for name in CHECKS),
        f"{label}: all capacity checks must be true",
    )
    # RSS is deliberately observational: allocator/kernel retention is recorded
    # but does not define a performance or correctness threshold.
    require(threads <= 64, f"{label}: implausible worker count")
    return document


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate GameNet capacity profile JSON")
    parser.add_argument("documents", nargs="+", type=Path)
    parser.add_argument("--expected-platform")
    parser.add_argument("--expected-backend")
    parser.add_argument("--expected-build-type")
    parser.add_argument("--expected-connections", type=int)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        for path in args.documents:
            document = (
                json.load(sys.stdin)
                if path == Path("-")
                else json.loads(path.read_text(encoding="utf-8"))
            )
            validate_document(
                document,
                expected_platform=args.expected_platform,
                expected_backend=args.expected_backend,
                expected_build_type=args.expected_build_type,
                expected_connections=args.expected_connections,
                label=str(path),
            )
    except (OSError, json.JSONDecodeError, CapacityProfileValidationError) as error:
        print(f"capacity profile validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
