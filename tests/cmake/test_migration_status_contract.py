from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from intent_inventory import IntentInventory, build_inventory


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing migration status fragment in {source}: {needle}"


def git(repo_root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    assert result.returncode == 0, (
        f"git {' '.join(args)} failed with {result.returncode}:\n{result.stderr}"
    )
    return result.stdout.strip()


def git_is_ancestor(
    repo_root: Path,
    ancestor: str,
    descendant: str,
) -> bool:
    result = subprocess.run(
        ["git", "merge-base", "--is-ancestor", ancestor, descendant],
        cwd=repo_root,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    assert result.returncode in {0, 1}, (
        "git merge-base --is-ancestor failed with "
        f"{result.returncode}:\n{result.stderr}"
    )
    return result.returncode == 0


def verify_current_intent_inventory(
    status_text: str,
    inventory: IntentInventory,
    source: Path,
) -> None:
    sections = re.findall(
        r"^## Current Intent Inventory\s*\n(.*?)(?=^## |\Z)",
        status_text,
        re.MULTILINE | re.DOTALL,
    )
    assert len(sections) == 1, (
        f"{source} must contain exactly one Current Intent Inventory section"
    )
    section = sections[0]
    normalized_section = " ".join(section.split())
    require(normalized_section, "`intents/README.md`", source)
    require(normalized_section, "front matter", source)
    rows = re.findall(
        r"^\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|$",
        section,
        re.MULTILINE,
    )
    assert len(rows) == 1, (
        f"{source} Current Intent Inventory must contain exactly one numeric data row"
    )
    actual = tuple(int(value) for value in rows[0])
    expected = (
        inventory.formal,
        inventory.active,
        inventory.deferred,
        inventory.legacy,
        inventory.verification_paths,
    )
    assert actual == expected, (
        "current intent inventory drift: "
        f"document={actual}, derived={expected}"
    )


def verify_inventory_tamper_detection(
    status_text: str,
    inventory: IntentInventory,
    source: Path,
) -> None:
    values = [
        inventory.formal,
        inventory.active,
        inventory.deferred,
        inventory.legacy,
        inventory.verification_paths,
    ]
    row = "| " + " | ".join(str(value) for value in values) + " |"
    assert status_text.count(row) == 1, (
        f"{source} must contain one canonical current inventory row"
    )
    for index, field in enumerate(
        ("formal", "active", "deferred", "legacy", "verification_paths")
    ):
        tampered_values = list(values)
        tampered_values[index] += 1
        tampered_row = (
            "| " + " | ".join(str(value) for value in tampered_values) + " |"
        )
        tampered_text = status_text.replace(row, tampered_row, 1)
        try:
            verify_current_intent_inventory(tampered_text, inventory, source)
        except AssertionError:
            continue
        raise AssertionError(
            f"migration status gate accepted tampered {field} inventory"
        )


def main() -> None:
    repo_root = REPO_ROOT
    migration_status = repo_root / "docs" / "migration_status.md"
    roadmap = repo_root / "docs" / "roadmap.md"
    assessment = repo_root / "assessment.md"
    plan = repo_root / "plan.md"
    goal = repo_root / "goal.md"
    readme = repo_root / "README.md"
    candidate_freeze = repo_root / "api" / "candidate_freeze.json"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"
    api_review = repo_root / "docs" / "reviews" / "api-r1-stable-core-review.md"
    perf_api_review = (
        repo_root / "docs" / "reviews" / "perf-r1-stable-core-additive-review.md"
    )
    historical_api_diff = repo_root / "docs" / "reviews" / "api-r1-public-api-diff.json"
    compatibility_api_diff = (
        repo_root / "docs" / "reviews" / "perf-r1-public-api-compatibility-diff.json"
    )
    perf_additive_api_diff = (
        repo_root / "docs" / "reviews" / "perf-r1-public-api-additive-diff.json"
    )
    common_profile_review = (
        repo_root
        / "docs"
        / "architecture"
        / "runtime_profile_common_capability_review.md"
    )
    x10_evidence_dir = (
        repo_root
        / "docs"
        / "development"
        / "benchmark_results"
        / "2026-08-22-ioe-x10-f5d39b8"
    )
    x10_evidence = x10_evidence_dir / "evidence.json"
    arch_g1_review = (
        repo_root / "docs" / "reviews" / "arch-g1-independent-review.md"
    )

    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    configured_tests = re.findall(
        r"^add_gamenet_(?:component_)?test\((unit|contract|integration)\s",
        tests_cmake_text,
        re.MULTILINE,
    )
    configured_test_count = len(configured_tests)
    unit_count = configured_tests.count("unit")
    contract_count = configured_tests.count("contract")
    integration_count = configured_tests.count("integration")
    assert configured_test_count > 0, "tests/CMakeLists.txt should configure CTest tests"

    status_text = migration_status.read_text(encoding="utf-8")
    roadmap_text = roadmap.read_text(encoding="utf-8")
    assessment_text = assessment.read_text(encoding="utf-8")
    plan_text = plan.read_text(encoding="utf-8")
    goal_text = goal.read_text(encoding="utf-8")
    readme_text = readme.read_text(encoding="utf-8")
    api_review_text = api_review.read_text(encoding="utf-8")
    perf_api_review_text = perf_api_review.read_text(encoding="utf-8")
    common_profile_review_text = common_profile_review.read_text(encoding="utf-8")
    x10_evidence_record = json.loads(x10_evidence.read_text(encoding="utf-8"))
    arch_g1_review_text = arch_g1_review.read_text(encoding="utf-8")
    freeze_record = json.loads(candidate_freeze.read_text(encoding="utf-8"))
    normalized_roadmap_text = " ".join(roadmap_text.split())
    normalized_plan_text = " ".join(plan_text.split())
    intent_inventory = build_inventory(repo_root)
    verify_current_intent_inventory(status_text, intent_inventory, migration_status)
    verify_inventory_tamper_detection(status_text, intent_inventory, migration_status)
    normalized_status_text = " ".join(status_text.split())
    require(status_text, "Last checked: 2026-07-11", migration_status)
    require(status_text, "Current production-roadmap audit: 2026-08-20", migration_status)
    require(status_text, "Current M1 closure audit: 2026-08-22", migration_status)
    implementation_checkpoint = "669ebb0a7c5c475dea74b12275c66a2ce1876804"
    superseded_candidate = "c061f9967b9481b70b2faf9a8fee24f5a3e72ffc"
    superseded_candidate_tag = "v0.3.0-rel-c1-refreeze-4"
    candidate_tag = "v0.3.0-rel-c1-refreeze-5"
    reviewed_surface_tag = "api-r1-perf-r1-reviewed-surface"
    reviewed_surface_commit = "6b292156e3e94d3389e9f3b8513445e7eb4ab541"
    current_governance_checkpoint = "a5ff7e6d823984a86e89146889f29f6615702ec3"
    current_implementation_checkpoint = "f5d39b800b4dd943531670aa09840c931c3dee4d"
    git(repo_root, "cat-file", "-e", f"{implementation_checkpoint}^{{commit}}")
    git(repo_root, "cat-file", "-e", f"{superseded_candidate}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", superseded_candidate, implementation_checkpoint)
    git(repo_root, "merge-base", "--is-ancestor", implementation_checkpoint, "HEAD")
    git(repo_root, "cat-file", "-e", f"{current_implementation_checkpoint}^{{commit}}")
    git(repo_root, "cat-file", "-e", f"{current_governance_checkpoint}^{{commit}}")
    git(
        repo_root,
        "merge-base",
        "--is-ancestor",
        current_governance_checkpoint,
        current_implementation_checkpoint,
    )
    git(repo_root, "merge-base", "--is-ancestor", current_implementation_checkpoint, "HEAD")

    assert x10_evidence_record["schema"] == "gamenet.ioe_x10_listener_evidence.v1"
    assert x10_evidence_record["decision"] == "PROMOTE"
    assert x10_evidence_record["decision_scope"] == (
        "later source-private io_uring integration shaping only"
    )
    assert x10_evidence_record["metadata"]["commit"] == current_implementation_checkpoint
    assert x10_evidence_record["protocol"] == {
        "active_routes": 256,
        "churn_waves": 4,
        "hub_route_limit": 256,
        "max_pending_accepts": 32,
        "payload_bytes": 64,
        "replacements_per_wave": 64,
        "round_trips_per_route_per_wave": 100,
    }
    for backend in ("epoll", "io_uring"):
        samples = x10_evidence_record["samples"][backend]
        assert len(samples) == 5
        for sample in samples:
            sample_path = x10_evidence_dir / sample["path"]
            assert sample_path.is_file(), sample_path
            digest = hashlib.sha256(sample_path.read_bytes()).hexdigest()
            assert digest == sample["sha256"]
    require(arch_g1_review_text, "`APPROVE`", arch_g1_review)
    require(arch_g1_review_text, current_implementation_checkpoint, arch_g1_review)

    assert freeze_record == {
        "schema": "gamenet.candidate_freeze.v1",
        "release_label": "v0.3.0-production-candidate",
        "stage": "rel-c1-refrozen-perf-r1-probe-lifecycle-remediation",
        "freeze_date": "2026-08-18",
        "candidate": {
            "branch": "perf-r1-deterministic-capacity",
            "ref": f"refs/tags/{candidate_tag}",
            "object_type": "annotated-tag",
            "commit_resolution": f"refs/tags/{candidate_tag}^{{commit}}",
            "sha_record": "annotated-tag-target-and-remote-ref",
        },
        "implementation_checkpoint": implementation_checkpoint,
        "supersedes": {
            "candidate_ref": f"refs/tags/{superseded_candidate_tag}",
            "candidate_commit": superseded_candidate,
            "rel_v2_run_id": "32043448820",
            "rel_v2_run_attempt": 1,
            "reason": "capacity-probe-lifecycle-barrier",
        },
        "reviewed_surface": {
            "tag": reviewed_surface_tag,
            "commit": reviewed_surface_commit,
            "snapshot": "api/baselines/v0.3.0-perf-r1-reviewed.json",
        },
        "policy": {
            "candidate_sha_is_not_duplicated_in_candidate_tree": True,
            "post_freeze_runtime_test_build_change_requires_refreeze": True,
            "local_evidence_is_not_remote_release_evidence": True,
            "release_decision": "pending-rel-d1",
        },
    }
    assert git(repo_root, "cat-file", "-t", f"refs/tags/{candidate_tag}") == "tag"
    candidate_sha = git(repo_root, "rev-parse", f"refs/tags/{candidate_tag}^{{commit}}")
    assert re.fullmatch(r"[0-9a-f]{40}", candidate_sha), candidate_sha
    assert git_is_ancestor(repo_root, candidate_sha, "HEAD"), (
        "historical REL-C1 tag must remain an ancestor of the checked-out commit"
    )
    require(
        normalized_status_text,
        "tag remains an ancestor of the checked-out commit",
        migration_status,
    )
    assert git(repo_root, "cat-file", "-t", f"refs/tags/{reviewed_surface_tag}") == "tag"
    assert (
        git(repo_root, "rev-parse", f"refs/tags/{reviewed_surface_tag}^{{commit}}")
        == reviewed_surface_commit
    )

    # The historical candidate remains immutable, but current development may
    # advance past it. New runtime changes are governed by intent/rules/tests
    # and exact-commit evidence rather than a post-checkpoint path allowlist.
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
    ):
        require(text, implementation_checkpoint, source)
        require(text, "API-R1", source)
        require(text, "REL-C1", source)
        require(text, candidate_tag, source)
        require(text, "REL-V1", source)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
    ):
        require(text, superseded_candidate, source)
        require(text, superseded_candidate_tag, source)
    require(readme_text, "669ebb0", readme)
    require(readme_text, superseded_candidate, readme)
    require(readme_text, superseded_candidate_tag, readme)
    require(readme_text, "API-R1", readme)
    require(readme_text, "REL-C1", readme)
    require(readme_text, candidate_tag, readme)
    require(
        status_text,
        "Historical v0.3 engineering candidate: the commit peeled from annotated tag",
        migration_status,
    )
    require(
        normalized_roadmap_text,
        "Candidate freeze is retired as a development gate",
        roadmap,
    )
    require(assessment_text, "IOE-X10 实现与证据提交 `f5d39b8`", assessment)
    require(assessment_text, "production-hardening preview", assessment)
    require(
        assessment_text,
        "IOE-X10 listener capacity/performance decision",
        assessment,
    )
    require(assessment_text, "ARCH-G1", assessment)
    require(assessment_text, "v0.3.0-internal-candidate", assessment)
    require(assessment_text, "gamenet-game-gateway", assessment)
    require(assessment_text, "NO-PROMOTION", assessment)
    require(assessment_text, "公共 backend selector", assessment)

    require(plan_text, current_governance_checkpoint, plan)
    require(plan_text, current_implementation_checkpoint, plan)
    require(plan_text, "# game-net-core 完整后续执行计划：IOE-X10 至 v1.0", plan)
    require(plan_text, "长期方向：`goal.md`", plan)
    require(plan_text, "当前评估：`assessment.md`", plan)
    require(plan_text, "当前唯一实现前沿是 **M2", plan)
    assert plan_text.count("当前唯一实现前沿") == 1, (
        "plan must declare exactly one current implementation front"
    )
    for milestone in (
        "M1：IOE-X10、ARCH-G1 与治理统一",
        "M2：v0.3.0 内部候选",
        "M3：`gamenet-game-gateway` 真实集成",
        "M4：Apache-2.0 下的 v0.3.0 外部发布",
        "M5：v0.4 Runtime 边界",
        "M6：v0.5 io_uring 可安装实验后端",
        "M7：v0.6 Lua 与 typed RPC",
        "M8：v0.7 Async 与 Coroutine",
        "M9：v0.8 TLS、Web" + "Socket 与 DNS",
        "M10：v0.9 UDP/KCP 实验能力",
        "M11：v1.0 稳定化与发布",
    ):
        require(plan_text, milestone, plan)
    roadmap_milestones = re.findall(
        r"^##\s+\d+\.\s+M(?:[1-9]|1[01])：",
        plan_text,
        re.MULTILINE,
    )
    assert len(roadmap_milestones) == 11, (
        "plan must contain exactly eleven executable roadmap milestones"
    )
    require(plan_text, "v0.3.0-internal-candidate", plan)
    require(plan_text, "gamenet-game-gateway", plan)
    require(plan_text, "NO-PROMOTION", plan)
    require(plan_text, "不开放公共 backend selector", plan)
    require(plan_text, "IOE-X1–X9", plan)
    require(plan_text, "X10=DEFER 或 ARCH-G1 要求暂停", plan)
    require(plan_text, "X10=PROMOTE 且 ARCH-G1=APPROVE", plan)
    require(plan_text, "M6 标记 skipped-by-evidence", plan)
    for slice_name in ("IOE-X11", "IOE-X12", "IOE-X13", "IOE-X14", "IOE-X15"):
        require(plan_text, slice_name, plan)
    require(plan_text, "GameNet::experimental_io_uring", plan)
    require(plan_text, "IoUringTcpServer", plan)
    require(plan_text, "IoUringTcpClient", plan)
    require(plan_text, "Apache-2.0", plan)
    require(plan_text, "GameNet::experimental_datagram", plan)
    require(plan_text, "v1 稳定范围", plan)
    require(plan_text, "v1 实验范围", plan)
    require(plan_text, "v1 发布门", plan)
    require(plan_text, "Linux/epoll 和 Windows/IOCP", plan)
    require(plan_text, "完整 HTTP server", plan)
    require(plan_text, "raw ICMP", plan)
    require(plan_text, "版本里程碑因证据分支被跳过时不发布空版本", plan)
    for fixed_protocol_anchor in (
        "并发 active routes | 256",
        "`maxPendingAccepts` | 32",
        "4 波，每波替换 64 routes",
        "每连接 echo round trips | 100",
        "payload | 64 bytes",
        "每个 backend 5 次 Release 样本",
    ):
        require(plan_text, fixed_protocol_anchor, plan)
    require(plan_text, "M1 状态：已关闭", plan)
    require(plan_text, "ARCH-G1：`APPROVE`", plan)
    require(plan_text, "2026-08-22-ioe-x10-f5d39b8", plan)
    assert "IOE-R2's generation-safe epoll Readiness Engine" not in plan_text
    assert "IOE-X8 cross-thread admission/lifecycle equivalence is next" not in plan_text
    require(
        tests_cmake_text,
        "integration/runtime_model/test_tcp_runtime_profiles.cpp",
        tests_cmake,
    )
    require(
        common_profile_review_text,
        "Do not promote an installed Runtime Profile interface now.",
        common_profile_review,
    )
    require(
        common_profile_review_text,
        "tests/integration/runtime_model/test_tcp_runtime_profiles.cpp",
        common_profile_review,
    )
    require(
        common_profile_review_text,
        "Disposition: `NO-PROMOTION`",
        common_profile_review,
    )
    require(status_text, "Current IOE-C1 typed operation-model checkpoint", migration_status)
    require(status_text, "Current IOE-C1 direct read/write checkpoint", migration_status)
    require(status_text, "Current IOE-C1 all-kind direct consumer checkpoint", migration_status)
    require(
        status_text,
        "Current IOE-C1 compatibility-path and shutdown closure checkpoint",
        migration_status,
    )
    require(
        status_text,
        "Current RTM-R1 Profile A SingleLoopInlineEvent checkpoint",
        migration_status,
    )
    require(
        status_text,
        "Current RTM-R1 Profile B MultiIoQueuedEvent checkpoint",
        migration_status,
    )
    require(
        status_text,
        "Current RTM-R1 Profile C MultiIoDedicatedFixedTick checkpoint",
        migration_status,
    )
    require(
        status_text,
        "`adb8b483d9b00ed0e9723321f2d7438e43a5e478`",
        migration_status,
    )
    require(
        normalized_roadmap_text,
        "IOE-X1 is closed at `d3b31c5c4e7966553094f7e42cf74f1b49a11077`",
        roadmap,
    )
    require(status_text, "kernel-terminal and consumer-terminal", migration_status)
    require(normalized_status_text, "The closure removes the remaining completion-to-Channel publisher", migration_status)
    require(normalized_status_text, "observably monotonic", migration_status)
    require(status_text, "Current API-R1 surface decision: `APPROVE`", migration_status)
    require(status_text, reviewed_surface_tag, migration_status)
    require(
        status_text,
        "The historical implementation checkpoint is `669ebb0`",
        migration_status,
    )
    require(goal_text, implementation_checkpoint, goal)
    require(goal_text, "本文不是当前实现授权，也不是发布证据", goal)
    require(goal_text, "owner-loop 并发与生命周期内核", goal)
    require(goal_text, "Readiness 与 Completion", goal)
    require(goal_text, "不把候选冻结或发布决定作为架构演进前置条件", goal)
    require(assessment_text, "外部采用仍被许可证阻塞", assessment)
    assert "stable surface review 未完成" not in assessment_text, (
        "assessment must not contradict the recorded API-R1 APPROVE decision"
    )
    assert "许可证和 API 审查共同阻塞" not in assessment_text, (
        "assessment must not list completed API review as a current blocker"
    )
    require(api_review_text, "Status: `approved-for-candidate-freeze`", api_review)
    require(api_review_text, "/root/api_r1_independent_review", api_review)
    require(api_review_text, "Changed stable-header fingerprints | 19", api_review)
    require(
        perf_api_review_text,
        "Status: `approved-additive-source-compatible`",
        perf_api_review,
    )
    require(perf_api_review_text, reviewed_surface_tag, perf_api_review)
    require(
        perf_api_review_text,
        "TcpConnection::setSendBufferSize",
        perf_api_review,
    )
    assert historical_api_diff.exists(), historical_api_diff
    assert compatibility_api_diff.exists(), compatibility_api_diff
    assert perf_additive_api_diff.exists(), perf_additive_api_diff
    assert "API-R1 stable-surface review is the next" not in readme_text
    assert "API-R1 is next" not in status_text
    require(
        normalized_roadmap_text,
        f"{configured_test_count} CTest tests: {unit_count} unit, "
        f"{contract_count} contract, and {integration_count} integration",
        roadmap,
    )
    require(
        assessment_text,
        f"默认测试基线为 {configured_test_count}",
        assessment,
    )
    require(assessment_text, "Linux experimental 基线为 136", assessment)
    require(
        normalized_plan_text,
        f"默认测试基线为 {configured_test_count}（{unit_count} unit、"
        f"{contract_count} contract、{integration_count} integration）",
        plan,
    )
    require(plan_text, "Linux experimental 基线为 136", plan)
    require(status_text, "gamenet.core_benchmark.v2", migration_status)
    require(status_text, "MetricsExporter is active but provisional", migration_status)
    require(
        status_text,
        "b3443182d0606792df44a12bcb08927e767bc060",
        migration_status,
    )
    require(status_text, "29895457789", migration_status)
    require(status_text, "73,617 cycles", migration_status)
    require(status_text, "29984629032", migration_status)
    require(status_text, "220,851 cycles", migration_status)
    require(status_text, "29808395220", migration_status)
    require(status_text, "earlier SHA `5f926f3`", migration_status)
    assert "real Linux/epoll duration evidence awaits" not in status_text
    require(status_text, f"{configured_test_count} configured CTest tests", migration_status)
    require(
        normalized_status_text,
        f"{unit_count} unit tests, {contract_count} contract tests, and {integration_count} integration test",
        migration_status,
    )
    require(status_text, "Last fully validated commit: `c4818d4b3956c85830e04d4a1f32df4ad701d453`", migration_status)
    require(status_text, "CI workflow run id: `29079836593` (`ci` #29, `main`)", migration_status)
    require(status_text, "Validation date: 2026-07-10", migration_status)
    require(status_text, "Release: annotated tag `v0.1.0-core-preview`", migration_status)
    require(status_text, "Focused candidate commit `a7fd77cbd2140041cebb3f900d5c609fafc2adad`", migration_status)
    require(status_text, "`29076601085` (#27)", migration_status)
    require(status_text, "preceding audited candidate", migration_status)
    require(status_text, "`d1474b5f32e609a7d2e2648af31b45635595d304`", migration_status)
    require(status_text, "`29073362905` (#26)", migration_status)
    require(status_text, "contract.tcp_client.test_tcp_client_repeated_connect", migration_status)
    require(status_text, "Linux CMake build and tests", migration_status)
    require(status_text, "Linux ASan/UBSan build and tests", migration_status)
    require(status_text, "Linux Release build", migration_status)
    require(status_text, "Windows MSVC IOCP build and tests", migration_status)
    require(status_text, "Linux TSan race-oriented build and tests", migration_status)
    require(status_text, "GAMENET_ENABLE_TSAN=ON", migration_status)
    require(status_text, "threading` label", migration_status)
    require(status_text, "pending read/write forceClose cancel-close", migration_status)
    require(status_text, "Latest recorded race-oriented CI remote green evidence is `ci` #29", migration_status)
    require(status_text, "intent consistency guard", migration_status)
    require(status_text, "intent metadata contract guard", migration_status)
    require(status_text, "connection_backpressure_controller", migration_status)
    require(status_text, "graceful_shutdown", migration_status)
    require(status_text, "global/per-peer connection limits", migration_status)
    require(status_text, "bounded fixed-window rate rejection", migration_status)
    require(status_text, "unauthenticated close", migration_status)
    require(status_text, "Production-hardening worktree Windows MSVC Debug AddressSanitizer", migration_status)
    require(status_text, "85/85 in 47.38 seconds", migration_status)
    require(status_text, "3,050/3,050 threading", migration_status)
    require(status_text, "400/400 Pipeline/Broadcast", migration_status)
    require(status_text, "All seven fixed Windows Release IOCP", migration_status)
    require(status_text, "bound to one frozen commit", migration_status)
    require(status_text, "promote_gate: never", migration_status)
    require(status_text, "Core benchmark contract guard", migration_status)
    require(status_text, "Logger thread-contract guard", migration_status)
    require(status_text, "TCP lifecycle contract guard", migration_status)
    require(status_text, "TcpConnection context contract guard", migration_status)
    require(status_text, "TcpConnection thread-contract guard", migration_status)
    require(status_text, "EventLoop contract guard", migration_status)
    require(status_text, "EventLoopThreadPool contract guard", migration_status)
    require(status_text, "TimerQueue contract guard", migration_status)
    require(status_text, "threading gate contract guard", migration_status)
    require(status_text, "MSVC UTF-8 build contract guard", migration_status)
    require(status_text, "Windows IOCP milestone contract guard", migration_status)
    require(status_text, "Windows IOCP data-path contract guard", migration_status)
    require(status_text, "Windows support is now represented by a `windows-msvc` workflow job", migration_status)
    require(status_text, "Windows install/package consumer gate also passes locally", migration_status)
    require(status_text, "find_package(GameNetCore)", migration_status)
    require(status_text, "GameNet::core", migration_status)
    require(status_text, "latest recorded green Windows job is `ci` #29", migration_status)
    require(status_text, "contract.timer_queue.test_timer_queue", migration_status)
    require(status_text, "server stop with active connections", migration_status)
    require(status_text, "server stop during active write", migration_status)
    require(status_text, "server stop soak for worker-owned connections", migration_status)
    require(status_text, "server multi-worker stop from the base loop", migration_status)
    require(status_text, "server worker-owned active-write stop", migration_status)
    require(status_text, "server worker-callback TcpServer stop soak", migration_status)
    require(status_text, "server repeated stop idempotence", migration_status)
    require(status_text, "contract.tcp_server.test_tcp_server_repeated_stop", migration_status)
    require(status_text, "client retry stop race", migration_status)
    require(status_text, "client retry-stop soak", migration_status)
    require(status_text, "direct Connector retry-stop cancellation", migration_status)
    require(status_text, "contract.connector.test_connector_retry_stop", migration_status)
    require(status_text, "client stop during pending ConnectEx", migration_status)
    require(status_text, "client pending ConnectEx stop soak", migration_status)
    require(status_text, "client cross-thread stop during pending ConnectEx", migration_status)
    require(status_text, "client mixed-timing pending ConnectEx stop soak", migration_status)
    require(status_text, "client destruction during pending ConnectEx", migration_status)
    require(status_text, "client destruction with active TcpConnection", migration_status)
    require(status_text, "client mixed-timing active-connection stop soak", migration_status)
    require(status_text, "client cross-thread active disconnect", migration_status)
    require(status_text, "client repeated active disconnect idempotence", migration_status)
    require(status_text, "client repeated active stop idempotence", migration_status)
    require(status_text, "contract.tcp_client.test_tcp_client_repeated_stop", migration_status)
    require(status_text, "client repeated active connect idempotence", migration_status)
    require(status_text, "contract.tcp_client.test_tcp_client_repeated_connect", migration_status)
    require(status_text, "client cross-thread active connect", migration_status)
    require(status_text, "client cross-thread retry configuration", migration_status)
    require(status_text, "contract.tcp_client.test_tcp_client_cross_thread_retry_config", migration_status)
    require(status_text, "peer close convergence", migration_status)
    require(status_text, "peer reset convergence", migration_status)
    require(status_text, "error-triggered teardown idempotence", migration_status)
    require(status_text, "cross-thread send delivery", migration_status)
    require(status_text, "concurrent Logger runtime-configuration coverage", migration_status)
    require(status_text, "contract.base.test_logger_thread_safety", migration_status)
    require(status_text, "cross-thread TcpConnection state observation", migration_status)
    require(status_text, "contract.tcp_connection.test_tcp_connection_cross_thread_state", migration_status)
    require(status_text, "post-close TcpConnection send ignore", migration_status)
    require(status_text, "contract.tcp_connection.test_tcp_connection_send_after_close", migration_status)
    require(status_text, "write-complete callback ordering", migration_status)
    require(status_text, "shutdown while output pending", migration_status)
    require(status_text, "cross-thread shutdown draining", migration_status)
    require(status_text, "repeated TcpConnection shutdown idempotence", migration_status)
    require(status_text, "contract.tcp_connection.test_tcp_connection_repeated_shutdown", migration_status)
    require(status_text, "high-water mark notification", migration_status)
    require(status_text, "repeated forceClose idempotence", migration_status)
    require(status_text, "repeated connectDestroyed stale-registration cleanup", migration_status)
    require(status_text, "contract.tcp_connection.test_tcp_connection_repeated_connect_destroyed", migration_status)
    require(status_text, "cross-thread forceClose soak", migration_status)
    require(status_text, "cross-thread pending-read forceClose", migration_status)
    require(status_text, "cross-thread pending-write forceClose", migration_status)
    require(status_text, "pending-read forceClose cancellation", migration_status)
    require(status_text, "mixed-timing pending-read forceClose soak", migration_status)
    require(status_text, "pending-write forceClose soak", migration_status)
    require(status_text, "mixed-timing pending-write forceClose soak", migration_status)
    require(status_text, "TimerQueue ready-timer cancellation race", migration_status)
    require(status_text, "EventLoopThreadPool queued-work soak", migration_status)
    require(status_text, "EventLoopThreadPool restart-stop soak", migration_status)
    require(status_text, "shared tests/support helpers", migration_status)
    require(status_text, "SocketPair.h", migration_status)
    require(status_text, "ClientSocket.h", migration_status)
    require(status_text, "centralizes nonblocking test-client connect and cleanup", migration_status)
    require(status_text, "Acceptor/Socket contracts", migration_status)
    require(status_text, "TcpConnection peer-reset setup", migration_status)
    require(status_text, "TcpConnectionHarness.h", migration_status)
    require(status_text, "centralizes loop-bound TcpConnection construction", migration_status)
    require(status_text, "TcpConnectionCallbacks.h", migration_status)
    require(status_text, "LoopTest.h", migration_status)
    require(status_text, "TcpClient lifecycle watchdogs", migration_status)
    require(status_text, "TcpServer lifecycle watchdogs", migration_status)
    require(status_text, "ThreadHandoff.h", migration_status)
    require(status_text, "centralizes one-shot and delayed non-owner-thread handoff", migration_status)
    require(status_text, "FutureTest.h", migration_status)
    require(status_text, "centralizes bounded future waits for EventLoop, EventLoopThread, TimerQueue, and EventLoopThreadPool async contract tests", migration_status)
    require(status_text, "TcpClientStopHarness.h", migration_status)
    require(status_text, "centralizes retry-stop stale-reconnect assertions", migration_status)
    require(status_text, "TcpServerHarness.h", migration_status)
    require(
        status_text,
        "centralizes multi-client TcpServer connection setup and worker-loop distribution assertions",
        migration_status,
    )
    require(status_text, "Local Windows Debug long-soak evidence also covers the previous", migration_status)
    require(status_text, "43-test threading slice", migration_status)
    require(status_text, "expanded the threading label to 44 tests", migration_status)
    require(status_text, "ctest --test-dir build -C Debug --output-on-failure -L threading --repeat until-fail:20 --timeout 60", migration_status)
    require(status_text, "43/43 threading-labeled tests passed across 20 repeats", migration_status)
    require(status_text, "CTest reported total test time was 637.56 seconds", migration_status)
    require(status_text, "44-test threading slice was covered once by the full Windows Debug and Release", migration_status)
    require(status_text, "then-current threading slice to 46 tests", migration_status)
    require(
        normalized_status_text,
        "passed all 46 threading tests across 5 repeats",
        migration_status,
    )
    require(status_text, "CTest reported 176.90 seconds", migration_status)
    require(
        normalized_status_text,
        "corresponding historical remote GitHub `long-soak` evidence is recorded",
        migration_status,
    )
    require(status_text, "`29077148022`", migration_status)
    require(status_text, "`86311227712`", migration_status)
    require(status_text, "`a7fd77cbd2140041cebb3f900d5c609fafc2adad`", migration_status)
    require(status_text, "repeat 50", migration_status)
    require(status_text, "timeout 60 seconds", migration_status)
    require(status_text, "2026-07-10T08:04:12Z", migration_status)
    require(status_text, "46/46 threading-labeled tests passed", migration_status)
    require(status_text, "1632.47 seconds", migration_status)
    require(status_text, "28m27s", migration_status)
    require(status_text, "Local Windows Release", migration_status)
    require(status_text, "evidence now also passes", migration_status)
    require(status_text, "cmake --build build-release --config Release --parallel", migration_status)
    require(status_text, "ctest --test-dir build-release -C Release --output-on-failure --timeout 10", migration_status)
    require(status_text, "67/67 Release tests passed", migration_status)
    require(status_text, "37.38 seconds", migration_status)
    require(status_text, "GAMENET_BUILD_BENCHMARKS", migration_status)
    require(status_text, "gamenet.core_benchmark.v1", migration_status)
    require(status_text, "51.979 MiB/s", migration_status)
    require(status_text, "73.994 MiB/s", migration_status)
    require(status_text, "71,264 bytes", migration_status)
    require(status_text, "67,465,216-byte", migration_status)
    require(status_text, "manual-only `core-benchmark` workflow", migration_status)
    require(status_text, "`29077151229`", migration_status)
    require(status_text, "`epoll_wait_batch`", migration_status)
    require(status_text, "32.337/65.234 MiB/s", migration_status)
    require(status_text, "16.188/26.110 MiB/s", migration_status)
    require(status_text, "All eight JSON files", migration_status)
    require(status_text, "install/package consumer also passes locally", migration_status)
    require(status_text, "build-release/_install", migration_status)
    require(status_text, "build-release-install-consumer", migration_status)
    require(status_text, "74/74 passed in 34.67 seconds", migration_status)
    require(status_text, "74/74 passed in 34.41 seconds", migration_status)
    require(status_text, "## Phase 4 Implementation State", migration_status)
    require(
        status_text,
        "The Phase 4 entry evidence gate was satisfied by the annotated preview tag",
        migration_status,
    )
    require(status_text, "no GitHub Release was published for that tag", migration_status)
    require(
        status_text,
        "Fresh candidate-SHA remote CI evidence is recorded for Linux CMake, Linux ASan/UBSan, Linux TSan, Linux Release, and Windows MSVC IOCP",
        migration_status,
    )
    require(
        status_text,
        "remote `long-soak` workflow has a green run recorded with run id, commit sha, repeat count, timeout, date, result, and duration",
        migration_status,
    )
    require(
        status_text,
        "`docs/migration_status.md`, `docs/development/ci.md`, and `docs/development/windows_iocp_milestone.md` have no pending evidence for the validated candidate",
        migration_status,
    )
    require(
        status_text,
        "TcpConnection, TcpClient/Connector, and TcpServer lifecycle/race tests have no known flaky entries",
        migration_status,
    )
    require(
        status_text,
        "Linux and Windows install/package consumers pass through `find_package(GameNetCore)` and `GameNet::core`",
        migration_status,
    )
    require(
        status_text,
        "Matching Release `gamenet.core_benchmark.v1` evidence is recorded for Linux",
        migration_status,
    )
    require(
        status_text,
        "PacketFramer has an approved active intent",
        migration_status,
    )
    require(
        status_text,
        "PR #2 is merged and annotated tag `v0.1.0-core-preview` points at the",
        migration_status,
    )
    require(status_text, "this was a tag-only preview, not a GitHub Release", migration_status)
    require(
        status_text,
        "HTTP, RPC, UDP/KCP, TLS, coroutine, and a formal all-in-one pipeline library",
        migration_status,
    )
    assert "Result: 21/21 tests passed" not in status_text, (
        "migration status must not present the old 21/21 result as the current worktree result"
    )
    assert "pending fresh Linux CI execution" not in status_text, (
        "migration status must not keep the pre-green CI state after the latest successful workflow run"
    )
    assert "remote green status is established by pull request or manual workflow execution" not in status_text, (
        "migration status must name the successful remote run instead of a future establishment path"
    )
    assert "ci #17" not in status_text, "migration status must not keep stale ci #17 as current evidence"
    assert "Remote green evidence for this new job is pending" not in status_text, (
        "migration status must not keep stale pending TSan evidence after ci #23 passed"
    )
    assert "Current result: Latest remote CI run:" not in status_text, (
        "migration status must distinguish latest recorded remote green evidence from latest HEAD evidence"
    )
    assert "Current HEAD:" not in status_text, (
        "migration status must use immutable validation records instead of a self-referential HEAD field"
    )
    assert "The latest HEAD has green remote CI evidence" not in status_text, (
        "migration status must not claim latest HEAD green until fresh evidence is recorded for that commit"
    )
    assert "remote green status is established by ci #23 on main" not in status_text, (
        "migration status must not present ci #23 as current-HEAD remote green evidence"
    )
    assert "remote `long-soak` workflow evidence remains" not in status_text, (
        "migration status must not keep stale pending long-soak evidence after run 28986707243 passed"
    )
    assert "pending until the manual workflow is run on GitHub" not in status_text, (
        "migration status must not say the manual long-soak is pending after run 28986707243 passed"
    )
    assert "fresh remote validation is still required" not in status_text, (
        "migration status must not retain the failed-candidate state after ci #27 passed"
    )
    assert "Linux Release JSON remains required" not in status_text, (
        "migration status must not retain local-only benchmark wording after run 29077151229 passed"
    )
    assert "not the required remote repeat-50 evidence" not in status_text, (
        "migration status must not retain pre-soak wording after run 29077148022 passed"
    )

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "python3 tests/cmake/test_migration_status_contract.py", workflow)

    ci_contract_text = ci_contract.read_text(encoding="utf-8")
    require(ci_contract_text, "python3 tests/cmake/test_migration_status_contract.py", ci_contract)

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, "## Remote Evidence Boundary", ci_docs)
    require(ci_docs_text, "Last fully validated commit: `c4818d4b3956c85830e04d4a1f32df4ad701d453`", ci_docs)
    require(ci_docs_text, "CI workflow run id: `29079836593` (`ci` #29, `main`)", ci_docs)
    require(ci_docs_text, "Release tag: `v0.1.0-core-preview`", ci_docs)
    require(ci_docs_text, "run `29076601085` (#27)", ci_docs)
    require(ci_docs_text, "run id `29073362905`", ci_docs)
    require(ci_docs_text, "contract.tcp_client.test_tcp_client_repeated_connect", ci_docs)
    require(ci_docs_text, "run `29077148022`", ci_docs)
    require(ci_docs_text, "run `29077151229`", ci_docs)
    require(ci_docs_text, "core-benchmark-linux-release-a7fd77cbd2140041cebb3f900d5c609fafc2adad", ci_docs)
    require(ci_docs_text, "core-benchmark-windows-release-a7fd77cbd2140041cebb3f900d5c609fafc2adad", ci_docs)
    assert "Current main HEAD" not in ci_docs_text, (
        "CI docs must not store a self-referential current-HEAD checkpoint"
    )


if __name__ == "__main__":
    main()
