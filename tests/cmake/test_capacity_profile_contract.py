from __future__ import annotations

import copy
import sys
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing capacity profile contract fragment in {source}: {needle}"


def fixed_storage(*, current: int, peak: int, read: int = 0) -> dict[str, int]:
    return {
        "shared_read_pool_bytes": 0,
        "shared_read_slab_bytes": 0,
        "accept_ex_fixed_pool_bytes": current - read,
        "iocp_completion_batch_bytes": 0,
        "connection_local_read_bytes": read,
        "total_retained_bytes": current,
        "peak_total_retained_bytes": peak,
        "accept_ex_slot_limit_per_acceptor": 64,
        "iocp_completion_batch_entries_per_loop": 64,
        "connection_local_read_chunk_limit_bytes": 4096,
    }


def retention() -> dict[str, int]:
    return {
        "input_buffer_bytes": 4128,
        "output_buffer_bytes": 4128,
        "transport_read_storage_bytes": 16384,
        "total_connection_bytes": 24640,
        "peak_input_buffer_bytes": 4128,
        "peak_output_buffer_bytes": 4128,
        "peak_transport_read_storage_bytes": 16384,
        "input_trim_count": 0,
        "output_trim_count": 0,
    }


def valid_document() -> dict[str, object]:
    reasons = {
        "none": 0,
        "offline_session": 0,
        "duplicate_endpoint": 0,
        "fanout_hard_limit": 0,
        "byte_hard_limit": 0,
        "low_priority_soft_limit": 0,
        "dispatch_task_byte_limit": 0,
        "endpoint_closed": 0,
        "endpoint_overloaded": 216,
        "owner_unavailable": 0,
        "owner_shutdown": 0,
        "dispatch_queue_full": 0,
        "owner_outstanding_task_limit": 0,
        "owner_outstanding_byte_limit": 0,
        "global_outstanding_byte_limit": 0,
        "invalid_plan": 0,
        "send_rejected": 0,
    }
    return {
        "schema": "gamenet.capacity_profile.v1",
        "status": "ok",
        "error": None,
        "scenario": "slow-broadcast-recovery",
        "platform": "windows",
        "backend": "iocp",
        "build_type": "Release",
        "parameters": {
            "connections": 4,
            "threads": 2,
            "messages": 64,
            "payload_bytes": 262144,
            "pressure_settle_ms": 500,
            "recovery_stable_ms": 250,
            "timeout_ms": 30000,
            "iocp_accept_depth": 8,
        },
        "limits": {
            "connection_low_water_bytes": 262144,
            "connection_high_water_bytes": 524288,
            "connection_hard_limit_bytes": 2097152,
            "aggregate_pending_hard_limit_bytes": 8388608,
            "broadcast_global_outstanding_limit_bytes": 67108864,
            "recovery_pending_threshold_bytes": 0,
            "buffer_max_retained_capacity_bytes": 65544,
        },
        "terminal": {
            "scheduled_endpoints": 256,
            "accepted_endpoints": 40,
            "dropped_endpoints": 216,
            "complete": True,
            "reasons": reasons,
            "tcp_rejections": {
                "connection": 216,
                "loop": 0,
                "server": 0,
                "global": 0,
                "total": 216,
            },
        },
        "pressure": {
            "elapsed_ms": 500.0,
            "pending_current_bytes": 8388608,
            "pending_peak_bytes": 8388608,
            "overloaded_connections": 4,
            "broadcast_outstanding_tasks": 0,
            "broadcast_outstanding_bytes": 0,
            "broadcast_peak_tasks": 21,
            "broadcast_peak_bytes": 11010048,
            "working_set_bytes": 15000000,
            "connection_retention": retention(),
            "fixed_storage": fixed_storage(current=26112, peak=26112, read=16384),
        },
        "recovery": {
            "elapsed_ms": 295.0,
            "stable_window_ms": 250,
            "pending_current_bytes": 0,
            "pending_peak_bytes": 8388608,
            "broadcast_outstanding_tasks": 0,
            "broadcast_outstanding_bytes": 0,
            "working_set_bytes": 7000000,
            "working_set_delta_from_baseline_bytes": 2000000,
            "connection_retention": retention(),
            "fixed_storage": fixed_storage(current=26112, peak=26112, read=16384),
        },
        "process": {
            "working_set_before_bytes": 5000000,
            "working_set_after_bytes": 6000000,
            "working_set_peak_bytes": 16000000,
            "client_received_bytes": 10485760,
            "fixed_storage_baseline": fixed_storage(current=9728, peak=9728),
            "fixed_storage_after_teardown": fixed_storage(current=0, peak=26112),
        },
        "checks": {
            "pending_within_limit": True,
            "broadcast_within_limit": True,
            "terminal_accounted": True,
            "client_delivery_accounted": True,
            "rejection_attributed": True,
            "overload_observed": True,
            "recovery_stable": True,
            "recovery_retained_within_target": True,
            "fixed_storage_coherent": True,
            "teardown_released": True,
            "passed": True,
        },
    }


def valid_mixed_document() -> dict[str, object]:
    document = copy.deepcopy(valid_document())
    document["schema"] = "gamenet.capacity_profile.v2"
    document["scenario"] = "mixed-pressure-recovery"
    document["parameters"].update(  # type: ignore[union-attr]
        {
            "probe_target_per_second": 100,
            "probe_duration_ms": 2000,
            "probe_batch_size": 10,
            "probe_concurrency": 4,
            "probe_payload_bytes": 32,
            "probe_connect_timeout_ms": 1000,
        }
    )
    document["healthy_churn"] = {
        "attempted": 200,
        "client_connected": 200,
        "server_accepted": 200,
        "probe_succeeded": 200,
        "server_closed": 200,
        "batches": 20,
        "elapsed_ms": 2001.0,
        "attempts_per_second": 99.950025,
        "connect_p99_us": 500.0,
        "probe_p99_us": 200.0,
        "schedule_lag_p99_us": 1000.0,
        "failures": {
            "connect": 0,
            "send": 0,
            "receive": 0,
            "payload_mismatch": 0,
            "total": 0,
        },
    }
    document["checks"].update(  # type: ignore[union-attr]
        {
            "healthy_probe_accounted": True,
            "healthy_probe_zero_failures": True,
            "healthy_probe_closed": True,
            "healthy_probe_paced": True,
        }
    )
    return document


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    benchmark_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    source = repo_root / "benchmarks" / "capacity" / "main.cpp"
    validator = repo_root / "tools" / "validate_capacity_profile.py"
    intent = repo_root / "intents" / "modules" / "broadcast.intent.md"
    testing_rules = repo_root / "rules" / "testing_rules.md"
    docs = repo_root / "docs" / "development" / "capacity_profile.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"

    cmake_text = benchmark_cmake.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    validator_text = validator.read_text(encoding="utf-8")
    intent_text = intent.read_text(encoding="utf-8")
    rules_text = testing_rules.read_text(encoding="utf-8")
    docs_text = docs.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")

    for fragment in (
        "add_executable(gamenet_capacity_profile",
        "GameNet::core",
        "GameNet::transport",
        "GameNet::broadcast",
        "GAMENET_BENCHMARK_BUILD_TYPE",
    ):
        require(cmake_text, fragment, benchmark_cmake)
    assert "add_test(" not in cmake_text, "capacity profiles must remain opt-in, not CTest"
    assert "install(" not in cmake_text, "capacity profiles must not enter the install surface"

    for fragment in (
        "gamenet.capacity_profile.v1",
        "slow-broadcast-recovery",
        "mixed-pressure-recovery",
        "TcpTransportEndpoint",
        "SO_RCVBUF",
        "aggregateRetention",
        "memoryRetentionSnapshot",
        "networkFixedStorageRetentionSnapshot",
        "EndpointOverloaded",
        "recoveryStable",
        "workingSetRecoveryDeltaBytes",
        "sampledWorkingSetPeak",
        "HealthyProbePool",
        "connectToWithDeadline",
        "healthyProbeAccounted",
    ):
        require(source_text, fragment, source)
    for fragment in (
        "gamenet.capacity_profile.v1",
        "gamenet.capacity_profile.v2",
        "EndpointOverloaded does not reconcile with TCP rejection scopes",
        "recovery did not sustain its stable window",
        "healthy probe accounting is inconsistent",
        "RSS is deliberately observational",
    ):
        require(validator_text, fragment, validator)
    require(intent_text, "gamenet_capacity_profile --scenario slow-broadcast-recovery", intent)
    require(rules_text, "the slow-broadcast-recovery capacity profile must use real TCP", testing_rules)
    for fragment in (
        "gamenet.capacity_profile.v1",
        "owner-loop-only",
        "RSS before/pressure/recovery/peak/after is observational",
        "2026-07-30 local M3-P1 seed",
        "accepted / `EndpointOverloaded`",
        "100/1k and 1k/10k+ scale",
        "M3-P1-D scale seed",
    ):
        require(docs_text, fragment, docs)
    require(
        workflow_text,
        "tests/cmake/test_capacity_profile_contract.py",
        workflow,
    )

    sys.path.insert(0, str(repo_root / "tools"))
    import validate_capacity_profile as capacity_validator

    document = valid_document()
    capacity_validator.validate_document(
        document,
        expected_platform="windows",
        expected_backend="iocp",
        expected_build_type="Release",
        expected_connections=4,
    )
    mixed_document = valid_mixed_document()
    capacity_validator.validate_document(
        mixed_document,
        expected_platform="windows",
        expected_backend="iocp",
        expected_build_type="Release",
        expected_connections=4,
    )

    mutations = (
        ("pending overflow", ("pressure", "pending_peak_bytes"), 8388609),
        ("broadcast overflow", ("pressure", "broadcast_peak_bytes"), 67108865),
        ("unstable recovery", ("recovery", "elapsed_ms"), 249.0),
        (
            "unattributed rejection",
            ("terminal", "reasons", "owner_unavailable"),
            1,
        ),
        ("fixed-storage leak", ("process", "fixed_storage_after_teardown", "total_retained_bytes"), 1),
        ("incoherent RSS peak", ("process", "working_set_peak_bytes"), 14999999),
        ("false producer check", ("checks", "passed"), False),
    )
    for label, path, value in mutations:
        mutated = copy.deepcopy(document)
        cursor = mutated
        for key in path[:-1]:
            cursor = cursor[key]  # type: ignore[index,assignment]
        cursor[path[-1]] = value  # type: ignore[index]
        try:
            capacity_validator.validate_document(mutated, label=label)
        except capacity_validator.CapacityProfileValidationError:
            pass
        else:
            raise AssertionError(f"validator accepted mutation: {label}")

    v1_probe_parameters = copy.deepcopy(document)
    v1_probe_parameters["parameters"]["probe_target_per_second"] = 100
    try:
        capacity_validator.validate_document(
            v1_probe_parameters,
            label="v1 probe parameter injection",
        )
    except capacity_validator.CapacityProfileValidationError:
        pass
    else:
        raise AssertionError("validator accepted v1 probe parameters")

    mixed_mutations = (
        ("probe attempt mismatch", ("healthy_churn", "attempted"), 199),
        ("probe batch mismatch", ("healthy_churn", "batches"), 19),
        ("probe close mismatch", ("healthy_churn", "server_closed"), 199),
        (
            "probe failure mismatch",
            ("healthy_churn", "failures", "total"),
            1,
        ),
        (
            "probe rate mismatch",
            ("healthy_churn", "attempts_per_second"),
            90.0,
        ),
        (
            "probe paced interval too short",
            ("healthy_churn", "elapsed_ms"),
            1990.0,
        ),
        (
            "probe P99 exceeds elapsed",
            ("healthy_churn", "connect_p99_us"),
            2_002_000.0,
        ),
        (
            "probe false check",
            ("checks", "healthy_probe_accounted"),
            False,
        ),
    )
    for label, path, value in mixed_mutations:
        mutated = copy.deepcopy(mixed_document)
        cursor = mutated
        for key in path[:-1]:
            cursor = cursor[key]  # type: ignore[index,assignment]
        cursor[path[-1]] = value  # type: ignore[index]
        try:
            capacity_validator.validate_document(mutated, label=label)
        except capacity_validator.CapacityProfileValidationError:
            pass
        else:
            raise AssertionError(f"validator accepted mixed mutation: {label}")


if __name__ == "__main__":
    main()
