# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing HP0 hot-path contract fragment in {source}: {needle}"


def main() -> None:
    repo = Path(__file__).resolve().parents[2]
    top_cmake = repo / "CMakeLists.txt"
    benchmark_cmake = repo / "benchmarks" / "CMakeLists.txt"
    inventory_path = repo / "benchmarks" / "hot_path_cost_inventory.json"
    intent_index = repo / "intents" / "README.md"
    intent = repo / "intents" / "usecases" / "hot_path_cost_baseline.intent.md"
    prestudy_intent = (
        repo / "intents" / "usecases" / "hp1_packet_framer_view_prestudy.intent.md"
    )
    hp2_intent = (
        repo / "intents" / "usecases" / "hp2_spsc_mailbox_prestudy.intent.md"
    )
    hp3_intent = (
        repo / "intents" / "usecases" / "hp3_epoll_slot_dispatch_prestudy.intent.md"
    )
    hp4_intent = (
        repo / "intents" / "usecases" / "hp4_output_segment_chain_prestudy.intent.md"
    )
    hp5_intent = (
        repo / "intents" / "usecases" / "hp5_credit_lease_prestudy.intent.md"
    )
    hp6_intent = (
        repo / "intents" / "usecases" / "hp6_adaptive_scheduler_prestudy.intent.md"
    )
    rules = repo / "rules" / "testing_rules.md"
    runner = repo / "tools" / "run_hot_path_cost.py"
    common = repo / "tools" / "hot_path_cost_common.py"
    validator = repo / "tools" / "validate_hot_path_cost.py"
    docs = repo / "docs" / "development" / "hot_path_cost_lab.md"
    async_semantics = repo / "intents" / "modules" / "async_semantics.intent.md"
    coroutine_task = repo / "intents" / "modules" / "coroutine_task.intent.md"
    async_timer = repo / "intents" / "modules" / "async_timer.intent.md"
    when_all = repo / "intents" / "modules" / "when_all.intent.md"
    when_any = repo / "intents" / "modules" / "when_any.intent.md"
    workflow = repo / ".github" / "workflows" / "hot-path-benchmark.yml"
    gitignore = repo / ".gitignore"

    top_text = top_cmake.read_text(encoding="utf-8")
    cmake_text = benchmark_cmake.read_text(encoding="utf-8")
    intent_text = intent.read_text(encoding="utf-8")
    prestudy_intent_text = prestudy_intent.read_text(encoding="utf-8")
    hp2_intent_text = hp2_intent.read_text(encoding="utf-8")
    hp3_intent_text = hp3_intent.read_text(encoding="utf-8")
    hp4_intent_text = hp4_intent.read_text(encoding="utf-8")
    hp5_intent_text = hp5_intent.read_text(encoding="utf-8")
    hp6_intent_text = hp6_intent.read_text(encoding="utf-8")
    rules_text = rules.read_text(encoding="utf-8")
    runner_text = runner.read_text(encoding="utf-8")
    common_text = common.read_text(encoding="utf-8")
    validator_text = validator.read_text(encoding="utf-8")
    docs_text = docs.read_text(encoding="utf-8")

    require(top_text, 'option(GAMENET_BUILD_BENCHMARKS "Build opt-in performance benchmarks" OFF)', top_cmake)
    require(top_text, "if(GAMENET_BUILD_BENCHMARKS)", top_cmake)
    require(cmake_text, "add_custom_target(gamenet_hot_path_benchmark", benchmark_cmake)
    for dependency in (
        "gamenet_core_benchmark",
        "gamenet_phase4_benchmark",
        "gamenet_capacity_profile",
        "gamenet_multi_io_queued_benchmark",
    ):
        require(cmake_text, dependency, benchmark_cmake)
    assert "add_test(" not in cmake_text, "hot-path suite must not be a CTest"
    assert "install(" not in cmake_text, "hot-path suite must not be installed"

    for fragment in (
        "add_library(gamenet_hp1_packet_framer_view STATIC",
        "add_executable(gamenet_hp1_packet_framer_view_benchmark",
        "add_custom_target(gamenet_hp1_packet_framer_view_prestudy",
        "GAMENET_BUILD_BENCHMARKS",
    ):
        require(top_text + cmake_text, fragment, benchmark_cmake)
    for fragment in (
        "status: active",
        "target: gamenet_hp1_packet_framer_view_benchmark",
        "promote_gate: none",
        "EXPERIMENTAL-PRESTUDY",
        "tests/contract/protocol/test_packet_framer_view.cpp",
        "KEEP-EXPERIMENTAL",
        "REJECT",
        "DEFER",
        "cannot switch Profile A",
        "Prestudy state: `KEEP-EXPERIMENTAL`",
    ):
        require(prestudy_intent_text, fragment, prestudy_intent)
    active_prestudies = []
    for candidate in (repo / "intents").rglob("*.intent.md"):
        candidate_text = candidate.read_text(encoding="utf-8")
        if (
            "status: active" in candidate_text
            and "target: gamenet_hp" in candidate_text
            and "Prestudy state: `ACTIVE`" in candidate_text
        ):
            active_prestudies.append(candidate.relative_to(repo).as_posix())
    assert len(active_prestudies) <= 1, (
        f"expected at most one active runtime prestudy, found {active_prestudies}"
    )
    require(
        (repo / "tests" / "CMakeLists.txt").read_text(encoding="utf-8"),
        "contract/protocol/test_packet_framer_view.cpp",
        repo / "tests" / "CMakeLists.txt",
    )
    assert not (repo / "include" / "gamenet" / "protocol" / "PacketView.h").exists()

    for fragment in (
        "add_library(gamenet_hp2_spsc_mailbox INTERFACE)",
        "add_executable(gamenet_hp2_spsc_mailbox_benchmark",
        "add_custom_target(gamenet_hp2_spsc_mailbox_prestudy",
    ):
        require(cmake_text, fragment, benchmark_cmake)
    for fragment in (
        "status: active",
        "target: gamenet_hp2_spsc_mailbox_benchmark",
        "promote_gate: none",
        "Prestudy state: `KEEP-EXPERIMENTAL`",
        "SpscMailboxMatrixPlan",
        "QueueFull",
        "Stopped",
        "OwnerUnavailable",
        "tests/contract/runtime_model/test_spsc_mailbox.cpp",
        "tests/contract/event_loop/test_event_loop_mailbox_source.cpp",
    ):
        require(hp2_intent_text, fragment, hp2_intent)
    tests_cmake_text = (repo / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    for fragment in (
        "contract/runtime_model/test_spsc_mailbox.cpp",
        "contract/event_loop/test_event_loop_mailbox_source.cpp",
    ):
        require(text=tests_cmake_text, needle=fragment, source=repo / "tests" / "CMakeLists.txt")
    assert not (repo / "include" / "gamenet" / "core" / "net" / "SpscMailbox.h").exists()

    for fragment in (
        "add_library(gamenet_hp3_epoll_slot_dispatch STATIC",
        "add_executable(gamenet_hp3_epoll_slot_dispatch_benchmark",
        "add_custom_target(gamenet_hp3_epoll_slot_dispatch_prestudy",
    ):
        require(cmake_text, fragment, benchmark_cmake)
    for fragment in (
        "status: active",
        "target: gamenet_hp3_epoll_slot_dispatch_benchmark",
        "promote_gate: none",
        "Prestudy state: `DEFER`",
        "slotIndex + generation",
        "lastBatchEpoch + noticeIndex",
        "tests/contract/io_engine/test_epoll_slot_dispatch.cpp",
        "native Linux",
    ):
        require(hp3_intent_text, fragment, hp3_intent)
    require(
        tests_cmake_text,
        "contract/io_engine/test_epoll_slot_dispatch.cpp",
        repo / "tests" / "CMakeLists.txt",
    )
    assert not (repo / "include" / "gamenet" / "core" / "net" / "EpollSlotArena.h").exists()

    for fragment in (
        "add_library(gamenet_hp4_output_segment_chain STATIC",
        "add_executable(gamenet_hp4_output_segment_chain_benchmark",
        "add_custom_target(gamenet_hp4_output_segment_chain_prestudy",
    ):
        require(cmake_text, fragment, benchmark_cmake)
    for fragment in (
        "status: active",
        "target: gamenet_hp4_output_segment_chain_benchmark",
        "promote_gate: none",
        "Prestudy state: `DEFER`",
        "at most 16 views / 64 KiB",
        "partial completion advances offsets",
        "tests/contract/tcp_connection/test_output_segment_chain.cpp",
        "invokes neither syscall",
    ):
        require(hp4_intent_text, fragment, hp4_intent)
    require(
        tests_cmake_text,
        "contract/tcp_connection/test_output_segment_chain.cpp",
        repo / "tests" / "CMakeLists.txt",
    )
    assert not (repo / "include" / "gamenet" / "core" / "net" / "OutputSegmentChain.h").exists()

    for fragment in (
        "add_library(gamenet_hp5_credit_lease STATIC",
        "add_executable(gamenet_hp5_credit_lease_benchmark",
        "add_custom_target(gamenet_hp5_credit_lease_prestudy",
    ):
        require(cmake_text, fragment, benchmark_cmake)
    for fragment in (
        "status: active",
        "target: gamenet_hp5_credit_lease_benchmark",
        "promote_gate: none",
        "Prestudy state: `KEEP-EXPERIMENTAL`",
        "connection -> loop -> server -> global",
        "Global rejection rolls back",
        "tests/contract/tcp_connection/test_credit_lease.cpp",
        "modeled shared atomic mutations",
    ):
        require(hp5_intent_text, fragment, hp5_intent)
    require(
        tests_cmake_text,
        "contract/tcp_connection/test_credit_lease.cpp",
        repo / "tests" / "CMakeLists.txt",
    )
    assert not (repo / "include" / "gamenet" / "core" / "net" / "CreditLease.h").exists()

    for fragment in (
        "add_library(gamenet_hp6_adaptive_scheduler STATIC",
        "add_executable(gamenet_hp6_adaptive_scheduler_benchmark",
        "add_custom_target(gamenet_hp6_adaptive_scheduler_prestudy",
    ):
        require(cmake_text, fragment, benchmark_cmake)
    for fragment in (
        "status: active",
        "target: gamenet_hp6_adaptive_scheduler_benchmark",
        "promote_gate: none",
        "Prestudy state: `DEFER`",
        "HP6-B pool work and HP6-C",
        "`SKIPPED-BY-EVIDENCE`",
        "tests/contract/event_loop/test_adaptive_phase_scheduler.cpp",
        "forceNonBlockingPoll",
    ):
        require(hp6_intent_text, fragment, hp6_intent)
    require(
        tests_cmake_text,
        "contract/event_loop/test_adaptive_phase_scheduler.cpp",
        repo / "tests" / "CMakeLists.txt",
    )
    assert not (repo / "include" / "gamenet" / "core" / "net" / "AdaptivePhaseScheduler.h").exists()

    assert "gamenet_hp7" not in cmake_text, "HP7 launch gate forbids a runtime target"
    for source, fragment in (
        (async_semantics, "A borrowed `PacketView` may"),
        (coroutine_task, "owner-bound `OwnerTask<T>`"),
        (async_timer, "Shutdown cancellation participates in final drain"),
        (when_all, "origin owner's bounded ready queue"),
        (when_any, "losers must each reach one terminal settlement"),
    ):
        text = source.read_text(encoding="utf-8")
        require(text, "HP7 is `SKIPPED-BY-EVIDENCE`", source)
        require(text, fragment, source)

    require(intent_index.read_text(encoding="utf-8"), "intents/usecases/hot_path_cost_baseline.intent.md", intent_index)
    for fragment in (
        "status: active",
        "target: gamenet_hot_path_benchmark",
        "migration_source: native",
        "artifact_kind: benchmark",
        "gamenet.hot_path_cost.v1",
        "callback and mutex-backed",
        "strict UTF-8",
        "public runtime",
        "HP1 promotion and integration cannot start",
        "EXPERIMENTAL-PRESTUDY",
        "At most one HP1-HP8 implementation prototype",
        "tests/cmake/test_hot_path_benchmark_contract.py",
    ):
        require(intent_text, fragment, intent)
    for fragment in (
        "one unrecorded warmup",
        "least ten recorded samples",
        "exact clean",
        "synthetic zero",
        "native Linux/epoll",
        "native Windows/IOCP",
        "rejects WSL",
        "EXPERIMENTAL-PRESTUDY",
        "at most one HP1-HP8 runtime",
        "no prestudy result is grandfathered",
    ):
        require(rules_text, fragment, rules)

    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    assert inventory["schema"] == "gamenet.hot_path_cost_inventory.v1"
    assert [item["key"] for item in inventory["scenarios"]] == [
        "framing.callback-copy",
        "network-logic.callback-mutex",
        "readiness-send.callback-borrowed",
        "broadcast.current-owner-tasks",
        "connection-capacity.current",
        "overload-recovery.current",
    ]
    assert set().union(*(set(item["domains"]) for item in inventory["scenarios"])) >= {
        "framing",
        "network_logic",
        "readiness",
        "send",
        "broadcast",
        "connection_capacity",
        "overload_recovery",
        "shutdown",
    }
    assert all(item["primary"] and item["guardrails"] and item["required_costs"] for item in inventory["scenarios"])

    sys.path.insert(0, str(repo / "tools"))
    from hot_path_cost_common import (  # noqa: PLC0415
        COST_KEYS,
        HotPathCostError,
        load_inventory,
        load_observation,
    )

    loaded = load_inventory(inventory_path)
    assert tuple(loaded["cost_keys"]) == COST_KEYS
    with tempfile.TemporaryDirectory(prefix="gamenet-hot-path-observer-") as temporary:
        observation = Path(temporary) / "observation.json"
        observation.write_text(
            json.dumps({
                "schema": "gamenet.hot_path_observation.v1",
                "costs": {key: (0 if key == "cycles" else None) for key in COST_KEYS},
            }),
            encoding="utf-8",
        )
        parsed = load_observation(observation)
        assert parsed["cycles"] == 0
        assert parsed["instructions"] is None
        invalid = json.loads(observation.read_text(encoding="utf-8"))
        invalid["costs"]["cycles"] = -1
        observation.write_text(json.dumps(invalid), encoding="utf-8")
        try:
            load_observation(observation)
        except HotPathCostError as error:
            assert "must be non-negative" in str(error)
        else:
            raise AssertionError("negative observer costs must fail closed")

    for source, fragments in (
        (common, ("LEDGER_SCHEMA", "COST_KEYS", "validate_child", "HotPathCostError")),
        (runner, ("scenario-round-robin", "rev-parse", "--porcelain=v1", "observer-command", "missing required costs", "backslashreplace")),
        (validator, ("--require-fixed-lab", "raw hash mismatch", "promotion eligibility", "parameters drifted")),
        (docs, (
            "Observer contract",
            "development-only",
            "self-hosted",
            "95% bootstrap",
            "Experimental prestudy while HP0 is open",
            "EXPERIMENTAL-PRESTUDY",
        )),
    ):
        text = source.read_text(encoding="utf-8")
        for fragment in fragments:
            require(text, fragment, source)

    for script in (runner, validator):
        result = subprocess.run(
            [sys.executable, str(script), "--help"],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, result.stderr

    invalid_manifest = subprocess.run(
        [
            sys.executable,
            str(validator),
            "--input",
            str(inventory_path),
        ],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
    )
    assert invalid_manifest.returncode != 0
    assert "ledger schema mismatch" in invalid_manifest.stderr
    assert workflow.is_file(), f"missing manual fixed-lab workflow: {workflow}"
    require(gitignore.read_text(encoding="utf-8"), "hot-path-results/", gitignore)


if __name__ == "__main__":
    main()
