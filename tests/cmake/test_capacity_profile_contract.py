from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
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


def valid_scale_ready_document() -> dict[str, object]:
    document = copy.deepcopy(valid_mixed_document())
    document["schema"] = "gamenet.capacity_profile.v3"
    document["parameters"]["reader_concurrency_limit"] = 16  # type: ignore[index]
    document["recovery_readers"] = {
        "workers": 4,
        "assigned_sockets": 4,
        "closed_sockets": 4,
    }
    document["checks"]["recovery_reader_pool_accounted"] = True  # type: ignore[index]
    return document


def capacity_gate_document(
    profile: object,
    *,
    platform: str,
    backend: str,
) -> dict[str, object]:
    document = valid_scale_ready_document()
    parameters = profile.parameters  # type: ignore[attr-defined]
    connections = int(parameters["connections"])
    messages = int(parameters["messages"])
    payload_bytes = int(parameters["payload_bytes"])
    endpoint_attempts = connections * messages
    dropped = connections
    accepted = endpoint_attempts - dropped
    document["platform"] = platform
    document["backend"] = backend
    document["parameters"] = {
        name: parameters[name]
        for name in (
            "connections",
            "threads",
            "messages",
            "payload_bytes",
            "pressure_settle_ms",
            "recovery_stable_ms",
            "timeout_ms",
            "iocp_accept_depth",
            "probe_target_per_second",
            "probe_duration_ms",
            "probe_batch_size",
            "probe_concurrency",
            "probe_payload_bytes",
            "probe_connect_timeout_ms",
            "reader_concurrency_limit",
        )
    }
    aggregate_limit = (
        connections
        * int(parameters["connection_hard_limit_bytes"])
    )
    document["limits"].update(  # type: ignore[union-attr]
        {
            "connection_low_water_bytes": parameters[
                "connection_low_water_bytes"
            ],
            "connection_high_water_bytes": parameters[
                "connection_high_water_bytes"
            ],
            "connection_hard_limit_bytes": parameters[
                "connection_hard_limit_bytes"
            ],
            "aggregate_pending_hard_limit_bytes": aggregate_limit,
            "broadcast_global_outstanding_limit_bytes": (
                endpoint_attempts * payload_bytes
            ),
            "recovery_pending_threshold_bytes": parameters[
                "recovery_pending_threshold_bytes"
            ],
        }
    )
    document["terminal"]["scheduled_endpoints"] = endpoint_attempts  # type: ignore[index]
    document["terminal"]["accepted_endpoints"] = accepted  # type: ignore[index]
    document["terminal"]["dropped_endpoints"] = dropped  # type: ignore[index]
    reasons = document["terminal"]["reasons"]  # type: ignore[index]
    for name in reasons:  # type: ignore[union-attr]
        reasons[name] = 0  # type: ignore[index]
    reasons["endpoint_overloaded"] = dropped  # type: ignore[index]
    document["terminal"]["tcp_rejections"] = {  # type: ignore[index]
        "connection": dropped,
        "loop": 0,
        "server": 0,
        "global": 0,
        "total": dropped,
    }
    document["pressure"]["pending_current_bytes"] = aggregate_limit  # type: ignore[index]
    document["pressure"]["pending_peak_bytes"] = aggregate_limit  # type: ignore[index]
    document["pressure"]["overloaded_connections"] = connections  # type: ignore[index]
    document["recovery"]["pending_peak_bytes"] = aggregate_limit  # type: ignore[index]
    document["process"]["client_received_bytes"] = accepted * payload_bytes  # type: ignore[index]

    probe_attempts = (
        int(parameters["probe_target_per_second"])
        * int(parameters["probe_duration_ms"])
        // 1000
    )
    probe_batches = (
        probe_attempts + int(parameters["probe_batch_size"]) - 1
    ) // int(parameters["probe_batch_size"])
    elapsed_ms = float(parameters["probe_duration_ms"]) + 1.0
    document["healthy_churn"].update(  # type: ignore[union-attr]
        {
            "attempted": probe_attempts,
            "client_connected": probe_attempts,
            "server_accepted": probe_attempts,
            "probe_succeeded": probe_attempts,
            "server_closed": probe_attempts,
            "batches": probe_batches,
            "elapsed_ms": elapsed_ms,
            "attempts_per_second": (
                probe_attempts / (elapsed_ms / 1000.0)
            ),
        }
    )
    document["recovery_readers"] = {
        "workers": min(
            int(parameters["reader_concurrency_limit"]),
            connections,
        ),
        "assigned_sockets": connections,
        "closed_sockets": connections,
    }
    return document


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_promotion_endurance_evidence(
    root: Path,
    *,
    mode: str,
    seconds: int,
    candidate_sha: str,
    workflow_run_id: str,
    workflow_run_attempt: int,
) -> Path:
    root.mkdir(parents=True)
    log = root / "fault-injection.log"
    log.write_text("promotion endurance fixture\n", encoding="utf-8")
    profiles = {
        name: 3
        for name in (
            "abrupt_peer_reset",
            "callback_exception",
            "output_overload",
            "healthy_recovery",
            "forced_shutdown",
        )
    }
    document = {
        "schema": "gamenet.production_endurance.v1",
        "status": "success",
        "mode": mode,
        "candidate_sha": candidate_sha,
        "workflow_run_id": workflow_run_id,
        "workflow_run_attempt": workflow_run_attempt,
        "platform": "linux",
        "backend": "epoll",
        "target_duration_seconds": seconds,
        "completed_cycles": 3,
        "child_elapsed_milliseconds": seconds * 1000,
        "profiles": profiles,
        "process_exit_code": 0,
        "memory": {
            "supported": True,
            "samples": 3,
            "first_rss_bytes": 20 * 1024 * 1024,
            "last_rss_bytes": 21 * 1024 * 1024,
            "minimum_rss_bytes": 20 * 1024 * 1024,
            "maximum_rss_bytes": 22 * 1024 * 1024,
            "rss_growth_bytes": 1024 * 1024,
            "max_rss_budget_bytes": 512 * 1024 * 1024,
            "max_rss_growth_budget_bytes": 64 * 1024 * 1024,
        },
        "test": {
            "name": "integration.resilience.test_fault_injection",
            "labels": [
                "endurance",
                "fault_injection",
                "threading",
                "lifecycle",
            ],
            "executable_sha256": "b" * 64,
        },
        "log": {
            "file": log.name,
            "bytes": log.stat().st_size,
            "sha256": sha256(log),
        },
    }
    result = root / "result.json"
    result.write_text(
        json.dumps(document, indent=2) + "\n",
        encoding="utf-8",
    )
    return result


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    benchmark_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    source = repo_root / "benchmarks" / "capacity" / "main.cpp"
    validator = repo_root / "tools" / "validate_capacity_profile.py"
    intent = repo_root / "intents" / "modules" / "broadcast.intent.md"
    release_intent = (
        repo_root
        / "intents"
        / "usecases"
        / "production_candidate_release.intent.md"
    )
    testing_rules = repo_root / "rules" / "testing_rules.md"
    docs = repo_root / "docs" / "development" / "capacity_profile.md"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    capacity_workflow = (
        repo_root / ".github" / "workflows" / "capacity-gate.yml"
    )
    gate_runner = repo_root / "tools" / "run_capacity_gate.py"
    pair_verifier = (
        repo_root / "tools" / "verify_capacity_gate_evidence_set.py"
    )
    promotion_verifier = (
        repo_root / "tools" / "verify_production_promotion_evidence.py"
    )

    cmake_text = benchmark_cmake.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    validator_text = validator.read_text(encoding="utf-8")
    intent_text = intent.read_text(encoding="utf-8")
    release_intent_text = release_intent.read_text(encoding="utf-8")
    rules_text = testing_rules.read_text(encoding="utf-8")
    docs_text = docs.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    capacity_workflow_text = capacity_workflow.read_text(
        encoding="utf-8"
    )
    gate_runner_text = gate_runner.read_text(encoding="utf-8")
    pair_verifier_text = pair_verifier.read_text(encoding="utf-8")
    promotion_verifier_text = promotion_verifier.read_text(
        encoding="utf-8"
    )

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
        "gamenet.capacity_profile.v3",
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
        "drainAvailable",
        "readerClosedSockets",
        "recoveryReaderPoolAccounted",
    ):
        require(source_text, fragment, source)
    for fragment in (
        "gamenet.capacity_profile.v1",
        "gamenet.capacity_profile.v2",
        "gamenet.capacity_profile.v3",
        "EndpointOverloaded does not reconcile with TCP rejection scopes",
        "recovery did not sustain its stable window",
        "healthy probe accounting is inconsistent",
        "recovery reader accounting is inconsistent",
        "RSS is deliberately observational",
    ):
        require(validator_text, fragment, validator)
    require(intent_text, "gamenet_capacity_profile --scenario slow-broadcast-recovery", intent)
    require(rules_text, "the slow-broadcast-recovery capacity profile must use real TCP", testing_rules)
    require(
        intent_text,
        "fixed-size recovery-reader pool",
        intent,
    )
    require(
        release_intent_text,
        "10k mixed slow-reader/Broadcast capacity evidence",
        release_intent,
    )
    require(
        release_intent_text,
        "candidate promotion revalidates the 10k pair",
        release_intent,
    )
    require(
        rules_text,
        "candidate versus 100k dedicated endpoint-attempt",
        testing_rules,
    )
    require(
        rules_text,
        "a production promotion artifact must revalidate",
        testing_rules,
    )
    for fragment in (
        "gamenet.capacity_profile.v1",
        "owner-loop-only",
        "RSS before/pressure/recovery/peak/after is observational",
        "2026-07-30 local M3-P1 seed",
        "accepted / `EndpointOverloaded`",
        "100/1k and 1k/10k+ scale",
        "M3-P1-D scale seed",
        "gamenet.capacity_profile.v3",
        "fixed-size recovery-reader pool",
    ):
        require(docs_text, fragment, docs)
    require(
        workflow_text,
        "tests/cmake/test_capacity_profile_contract.py",
        workflow,
    )
    for fragment in (
        "name: capacity-gate",
        "workflow_dispatch:",
        "candidate-10k",
        "dedicated-100k",
        "RUN_DEDICATED_100K",
        '["self-hosted","linux","x64","gamenet-endurance"]',
        '["self-hosted","windows","x64","gamenet-windows"]',
        "tools/run_capacity_gate.py",
        "tools/verify_capacity_gate_evidence_set.py",
        "Require successful capacity producers",
        "capacity-gate-pair-",
    ):
        require(capacity_workflow_text, fragment, capacity_workflow)
    assert "\n  push:" not in capacity_workflow_text
    assert "\n  pull_request:" not in capacity_workflow_text
    for fragment in (
        "gamenet.capacity_gate.v1",
        '"candidate-10k"',
        '"dedicated-100k"',
        "connections=1000",
        "connections=10000",
        "validate_gate_document",
        "capacity gate requires the scale-ready v3 schema",
    ):
        require(gate_runner_text, fragment, gate_runner)
    for fragment in (
        "gamenet.capacity_gate_pair.v1",
        "Linux/Windows capacity identity mismatch",
        "capacity parameters drifted from the reviewed profile",
        "capacity repetition contract drifted",
    ):
        require(pair_verifier_text, fragment, pair_verifier)
    for fragment in (
        "gamenet.production_promotion_evidence.v1",
        "capacity pair manifest does not match revalidated raw evidence",
        "capacity profile does not satisfy the promotion stage",
        "release promotion requires candidate-24h evidence",
    ):
        require(
            promotion_verifier_text,
            fragment,
            promotion_verifier,
        )

    sys.path.insert(0, str(repo_root / "tools"))
    import validate_capacity_profile as capacity_validator
    import run_capacity_gate as capacity_gate
    import verify_capacity_gate_evidence_set as capacity_pair
    import verify_production_promotion_evidence as promotion

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
    scale_ready_document = valid_scale_ready_document()
    capacity_validator.validate_document(
        scale_ready_document,
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

    scale_ready_mutations = (
        (
            "reader worker mismatch",
            ("recovery_readers", "workers"),
            3,
        ),
        (
            "reader assignment mismatch",
            ("recovery_readers", "assigned_sockets"),
            3,
        ),
        (
            "reader close mismatch",
            ("recovery_readers", "closed_sockets"),
            3,
        ),
        (
            "reader false check",
            ("checks", "recovery_reader_pool_accounted"),
            False,
        ),
    )
    for label, path, value in scale_ready_mutations:
        mutated = copy.deepcopy(scale_ready_document)
        cursor = mutated
        for key in path[:-1]:
            cursor = cursor[key]  # type: ignore[index,assignment]
        cursor[path[-1]] = value  # type: ignore[index]
        try:
            capacity_validator.validate_document(mutated, label=label)
        except capacity_validator.CapacityProfileValidationError:
            pass
        else:
            raise AssertionError(
                f"validator accepted scale-ready mutation: {label}"
            )

    candidate_profile = capacity_gate.PROFILES["candidate-10k"]
    dedicated_profile = capacity_gate.PROFILES["dedicated-100k"]
    assert candidate_profile.endpoint_attempts == 10_000
    assert candidate_profile.repetitions == 3
    assert candidate_profile.parameters["connections"] == 1_000
    assert candidate_profile.parameters["reader_concurrency_limit"] == 16
    assert dedicated_profile.endpoint_attempts == 100_000
    assert dedicated_profile.repetitions == 1
    assert dedicated_profile.parameters["connections"] == 10_000
    assert dedicated_profile.parameters["reader_concurrency_limit"] == 64

    with tempfile.TemporaryDirectory(
        prefix="gamenet-capacity-pair-"
    ) as directory:
        evidence_root = Path(directory)
        candidate_capacity_root = evidence_root / "candidate-capacity"
        candidate_capacity_root.mkdir()
        candidate_sha = "a" * 40
        run_id = "12345"
        run_attempt = 2
        fixtures: dict[
            str,
            tuple[Path, dict[str, object], dict[str, object]],
        ] = {}
        for job, platform, backend in (
            ("linux-capacity-gate", "linux", "epoll"),
            ("windows-capacity-gate", "windows", "iocp"),
        ):
            artifact_name = (
                f"capacity-gate-candidate-10k-{job}-"
                f"{candidate_sha}-{run_id}-{run_attempt}"
            )
            root = candidate_capacity_root / artifact_name
            root.mkdir()
            toolchain = root / "toolchain.txt"
            toolchain.write_text(
                f"{platform} fixture toolchain\n",
                encoding="utf-8",
            )
            samples: list[dict[str, object]] = []
            first_document: dict[str, object] | None = None
            for repetition in range(
                1,
                candidate_profile.repetitions + 1,
            ):
                document = capacity_gate_document(
                    candidate_profile,
                    platform=platform,
                    backend=backend,
                )
                if first_document is None:
                    first_document = copy.deepcopy(document)
                path = root / f"sample-{repetition}.json"
                path.write_text(
                    json.dumps(document, indent=2) + "\n",
                    encoding="utf-8",
                )
                samples.append(
                    {
                        "repetition": repetition,
                        "path": path.name,
                        "bytes": path.stat().st_size,
                        "sha256": sha256(path),
                    }
                )
            manifest: dict[str, object] = {
                "schema": capacity_gate.SCHEMA,
                "result": "pass",
                "profile": candidate_profile.name,
                "candidate_sha": candidate_sha,
                "run_id": run_id,
                "run_attempt": run_attempt,
                "job": job,
                "artifact_name": artifact_name,
                "platform": platform,
                "backend": backend,
                "build_type": "Release",
                "repetitions": candidate_profile.repetitions,
                "endpoint_attempts": (
                    candidate_profile.endpoint_attempts
                ),
                "probe_attempts": candidate_profile.probe_attempts,
                "parameters": candidate_profile.parameters,
                "executable_sha256": "e" * 64,
                "toolchain": {
                    "path": toolchain.name,
                    "bytes": toolchain.stat().st_size,
                    "sha256": sha256(toolchain),
                },
                "samples": samples,
            }
            (root / "capacity-manifest.json").write_text(
                json.dumps(manifest, indent=2) + "\n",
                encoding="utf-8",
            )
            assert first_document is not None
            fixtures[job] = (root, manifest, first_document)

        pair = capacity_pair.verify_evidence_set(
            candidate_capacity_root
        )
        assert pair["schema"] == capacity_pair.SCHEMA
        assert pair["result"] == "pass"
        assert pair["profile"] == "candidate-10k"
        assert pair["endpoint_attempts"] == 10_000
        assert len(pair["platforms"]) == 2
        (candidate_capacity_root / "pair-manifest.json").write_text(
            json.dumps(pair, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        capacity_pair.verify_retained_pair_manifest(
            candidate_capacity_root / "pair-manifest.json",
            pair,
        )
        capacity_pair.verify_expected_identity(
            pair,
            profile="candidate-10k",
            candidate_sha=candidate_sha,
            run_id=run_id,
            run_attempt=run_attempt,
        )
        try:
            capacity_pair.verify_expected_identity(
                pair,
                profile="candidate-10k",
                candidate_sha=candidate_sha,
                run_id=run_id,
                run_attempt=run_attempt + 1,
            )
        except capacity_pair.CapacityGatePairError:
            pass
        else:
            raise AssertionError(
                "capacity identity verifier accepted attempt drift"
            )

        candidate_endurance = write_promotion_endurance_evidence(
            evidence_root / "candidate-endurance",
            mode="candidate-24h",
            seconds=86_400,
            candidate_sha=candidate_sha,
            workflow_run_id="54321",
            workflow_run_attempt=1,
        )
        candidate_promotion = promotion.verify_promotion(
            stage="candidate",
            capacity_root=candidate_capacity_root,
            endurance_evidence=candidate_endurance,
            candidate_sha=candidate_sha,
            capacity_run_id=run_id,
            capacity_run_attempt=run_attempt,
            promotion_run_id="54321",
            promotion_run_attempt=1,
        )
        assert candidate_promotion["schema"] == promotion.SCHEMA
        assert candidate_promotion["stage"] == "candidate"
        assert (
            candidate_promotion["capacity"]["profile"]
            == "candidate-10k"
        )
        assert len(candidate_promotion["endurance"]) == 1

        dedicated_capacity_root = (
            evidence_root / "dedicated-capacity"
        )
        dedicated_capacity_root.mkdir()
        dedicated_run_id = "67890"
        dedicated_run_attempt = 3
        for job, platform, backend in (
            ("linux-capacity-gate", "linux", "epoll"),
            ("windows-capacity-gate", "windows", "iocp"),
        ):
            artifact_name = (
                f"capacity-gate-dedicated-100k-{job}-"
                f"{candidate_sha}-{dedicated_run_id}-"
                f"{dedicated_run_attempt}"
            )
            root = dedicated_capacity_root / artifact_name
            root.mkdir()
            toolchain = root / "toolchain.txt"
            toolchain.write_text(
                f"{platform} dedicated fixture toolchain\n",
                encoding="utf-8",
            )
            document = capacity_gate_document(
                dedicated_profile,
                platform=platform,
                backend=backend,
            )
            sample_path = root / "sample-1.json"
            sample_path.write_text(
                json.dumps(document, indent=2) + "\n",
                encoding="utf-8",
            )
            manifest = {
                "schema": capacity_gate.SCHEMA,
                "result": "pass",
                "profile": dedicated_profile.name,
                "candidate_sha": candidate_sha,
                "run_id": dedicated_run_id,
                "run_attempt": dedicated_run_attempt,
                "job": job,
                "artifact_name": artifact_name,
                "platform": platform,
                "backend": backend,
                "build_type": "Release",
                "repetitions": dedicated_profile.repetitions,
                "endpoint_attempts": (
                    dedicated_profile.endpoint_attempts
                ),
                "probe_attempts": dedicated_profile.probe_attempts,
                "parameters": dedicated_profile.parameters,
                "executable_sha256": "d" * 64,
                "toolchain": {
                    "path": toolchain.name,
                    "bytes": toolchain.stat().st_size,
                    "sha256": sha256(toolchain),
                },
                "samples": [
                    {
                        "repetition": 1,
                        "path": sample_path.name,
                        "bytes": sample_path.stat().st_size,
                        "sha256": sha256(sample_path),
                    }
                ],
            }
            (root / "capacity-manifest.json").write_text(
                json.dumps(manifest, indent=2) + "\n",
                encoding="utf-8",
            )
        dedicated_pair = capacity_pair.verify_evidence_set(
            dedicated_capacity_root
        )
        (
            dedicated_capacity_root / "pair-manifest.json"
        ).write_text(
            json.dumps(dedicated_pair, indent=2, sort_keys=True)
            + "\n",
            encoding="utf-8",
        )
        release_endurance = write_promotion_endurance_evidence(
            evidence_root / "release-endurance",
            mode="release-72h",
            seconds=259_200,
            candidate_sha=candidate_sha,
            workflow_run_id="60002",
            workflow_run_attempt=4,
        )
        release_promotion = promotion.verify_promotion(
            stage="release",
            capacity_root=dedicated_capacity_root,
            endurance_evidence=release_endurance,
            candidate_sha=candidate_sha,
            capacity_run_id=dedicated_run_id,
            capacity_run_attempt=dedicated_run_attempt,
            promotion_run_id="60002",
            promotion_run_attempt=4,
            candidate_endurance_evidence=candidate_endurance,
            candidate_endurance_run_id="54321",
            candidate_endurance_run_attempt=1,
        )
        assert release_promotion["stage"] == "release"
        assert (
            release_promotion["capacity"]["profile"]
            == "dedicated-100k"
        )
        assert [
            item["mode"]
            for item in release_promotion["endurance"]
        ] == ["candidate-24h", "release-72h"]

        def expect_promotion_failure(
            label: str,
            **overrides: object,
        ) -> None:
            arguments: dict[str, object] = {
                "stage": "candidate",
                "capacity_root": candidate_capacity_root,
                "endurance_evidence": candidate_endurance,
                "candidate_sha": candidate_sha,
                "capacity_run_id": run_id,
                "capacity_run_attempt": run_attempt,
                "promotion_run_id": "54321",
                "promotion_run_attempt": 1,
            }
            arguments.update(overrides)
            try:
                promotion.verify_promotion(**arguments)
            except (
                promotion.PromotionEvidenceError,
                promotion.EnduranceEvidenceError,
                capacity_pair.CapacityGatePairError,
            ):
                pass
            else:
                raise AssertionError(
                    f"promotion verifier accepted {label}"
                )

        expect_promotion_failure(
            "capacity source attempt drift",
            capacity_run_attempt=run_attempt + 1,
        )
        expect_promotion_failure(
            "endurance source attempt drift",
            promotion_run_attempt=2,
        )
        expect_promotion_failure(
            "candidate capacity used for release",
            stage="release",
            candidate_endurance_evidence=candidate_endurance,
            candidate_endurance_run_id="54321",
            candidate_endurance_run_attempt=1,
        )
        candidate_pair_path = (
            candidate_capacity_root / "pair-manifest.json"
        )
        drifted_pair = copy.deepcopy(pair)
        drifted_pair["probe_attempts"] = 499
        candidate_pair_path.write_text(
            json.dumps(drifted_pair, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        expect_promotion_failure("copied pair summary drift")
        candidate_pair_path.write_text(
            json.dumps(pair, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

        def expect_pair_failure(label: str) -> None:
            try:
                capacity_pair.verify_evidence_set(
                    candidate_capacity_root
                )
            except capacity_pair.CapacityGatePairError:
                pass
            else:
                raise AssertionError(
                    f"paired capacity verifier accepted {label}"
                )

        windows_root, windows_manifest, windows_document = fixtures[
            "windows-capacity-gate"
        ]
        drifted_manifest = copy.deepcopy(windows_manifest)
        drifted_manifest["parameters"]["connections"] = 999  # type: ignore[index]
        (windows_root / "capacity-manifest.json").write_text(
            json.dumps(drifted_manifest, indent=2) + "\n",
            encoding="utf-8",
        )
        expect_pair_failure("parameter drift")
        (windows_root / "capacity-manifest.json").write_text(
            json.dumps(windows_manifest, indent=2) + "\n",
            encoding="utf-8",
        )

        legacy_document = copy.deepcopy(windows_document)
        legacy_document["schema"] = "gamenet.capacity_profile.v2"
        sample_path = windows_root / "sample-1.json"
        sample_path.write_text(
            json.dumps(legacy_document, indent=2) + "\n",
            encoding="utf-8",
        )
        legacy_manifest = copy.deepcopy(windows_manifest)
        legacy_manifest["samples"][0]["bytes"] = sample_path.stat().st_size  # type: ignore[index]
        legacy_manifest["samples"][0]["sha256"] = sha256(sample_path)  # type: ignore[index]
        (windows_root / "capacity-manifest.json").write_text(
            json.dumps(legacy_manifest, indent=2) + "\n",
            encoding="utf-8",
        )
        expect_pair_failure("legacy v2 sample")
        sample_path.write_text(
            json.dumps(windows_document, indent=2) + "\n",
            encoding="utf-8",
        )
        (windows_root / "capacity-manifest.json").write_text(
            json.dumps(windows_manifest, indent=2) + "\n",
            encoding="utf-8",
        )

        bad_hash_manifest = copy.deepcopy(windows_manifest)
        bad_hash_manifest["samples"][0]["sha256"] = "0" * 64  # type: ignore[index]
        (windows_root / "capacity-manifest.json").write_text(
            json.dumps(bad_hash_manifest, indent=2) + "\n",
            encoding="utf-8",
        )
        expect_pair_failure("sample hash tampering")


if __name__ == "__main__":
    main()
