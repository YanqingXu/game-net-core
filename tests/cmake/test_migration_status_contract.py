# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

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
    profile_load_guide = (
        repo_root
        / "docs"
        / "architecture"
        / "runtime_profile_load_selection_guide.md"
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
    evidence_ledger = (
        repo_root / "docs" / "development" / "commit_bound_evidence_ledger.md"
    )
    m7_readiness = (
        repo_root
        / "docs"
        / "development"
        / "m7_external_rpc_lua_readiness_2026-08-24.md"
    )
    m8_readiness = (
        repo_root
        / "docs"
        / "development"
        / "m8_async_coroutine_readiness_2026-08-24.md"
    )
    rpc_intent = repo_root / "intents" / "modules" / "rpc.intent.md"
    async_intent_names = (
        "async_semantics",
        "coroutine_task",
        "async_timer",
        "connection_awaiter_registry",
        "when_all",
        "when_any",
    )
    async_intents = {
        name: repo_root / "intents" / "modules" / f"{name}.intent.md"
        for name in async_intent_names
    }
    intents_index = repo_root / "intents" / "README.md"

    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    configured_tests = re.findall(
        r"^add_gamenet_(?:component_)?test\((unit|contract|integration)\s",
        tests_cmake_text,
        re.MULTILINE,
    )
    cross_backend_test = "NAME contract.io_engine.test_cross_backend_tcp_semantics"
    assert tests_cmake_text.count(cross_backend_test) == 1, (
        "the manually linked cross-backend semantic contract must be registered once"
    )
    configured_tests.append("contract")
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
    profile_load_guide_text = profile_load_guide.read_text(encoding="utf-8")
    x10_evidence_record = json.loads(x10_evidence.read_text(encoding="utf-8"))
    arch_g1_review_text = arch_g1_review.read_text(encoding="utf-8")
    evidence_ledger_text = evidence_ledger.read_text(encoding="utf-8")
    m7_readiness_text = m7_readiness.read_text(encoding="utf-8")
    m8_readiness_text = m8_readiness.read_text(encoding="utf-8")
    rpc_intent_text = rpc_intent.read_text(encoding="utf-8")
    async_intent_texts = {
        name: path.read_text(encoding="utf-8")
        for name, path in async_intents.items()
    }
    intents_index_text = intents_index.read_text(encoding="utf-8")
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
    m5_decision_checkpoint = "f8cffb6f04e593983db16d23122ed426f8729bf4"
    x11_implementation_checkpoint = "013fecfe81277845eb3e60ccf5fe0205b753858d"
    x12_implementation_checkpoint = "5be30e701c61f8d6700bcc4be6bc0ef152120fb8"
    x13_implementation_checkpoint = "5484d7a89b01597824bc860e4d2d3cf3cfd45a82"
    x14_implementation_checkpoint = "351b3c0e476a462265016d53361a02b5f2c51611"
    x15_implementation_checkpoint = "43795e841ba2a279ed6a3d5d831d60a9f2a25570"
    m7_readiness_checkpoint = "44493b1d37c16567990e1660153d6b0843a8eecc"
    gateway_m7_implementation = "e43393c85fa37604d340fe866610756c99f4fe4e"
    gateway_m7_closure = "588acd079be93de3e230ba4f07dd111f7bec6a3c"
    m8_core_baseline = "e5ea9efa71dbe52e841423ec3cac3e9529158b22"
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
    git(repo_root, "cat-file", "-e", f"{m5_decision_checkpoint}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", m5_decision_checkpoint, "HEAD")
    git(repo_root, "cat-file", "-e", f"{x11_implementation_checkpoint}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", x11_implementation_checkpoint, "HEAD")
    git(repo_root, "cat-file", "-e", f"{x12_implementation_checkpoint}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", x12_implementation_checkpoint, "HEAD")
    git(repo_root, "cat-file", "-e", f"{x13_implementation_checkpoint}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", x13_implementation_checkpoint, "HEAD")
    git(repo_root, "cat-file", "-e", f"{x14_implementation_checkpoint}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", x14_implementation_checkpoint, "HEAD")
    git(repo_root, "cat-file", "-e", f"{x15_implementation_checkpoint}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", x15_implementation_checkpoint, "HEAD")
    git(repo_root, "cat-file", "-e", f"{m7_readiness_checkpoint}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", m7_readiness_checkpoint, "HEAD")
    git(repo_root, "cat-file", "-e", f"{m8_core_baseline}^{{commit}}")
    git(repo_root, "merge-base", "--is-ancestor", m8_core_baseline, "HEAD")

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
    require(assessment_text, "stable Apache-2.0 `v0.3.0@8e4a6ed`", assessment)
    require(assessment_text, "stable v0.3 基线", assessment)
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
    require(plan_text, m5_decision_checkpoint, plan)
    require(status_text, m5_decision_checkpoint, migration_status)
    require(evidence_ledger_text, m5_decision_checkpoint, evidence_ledger)
    require(evidence_ledger_text, "M5 Runtime Boundary Re-review", evidence_ledger)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
        (assessment_text, assessment),
        (plan_text, plan),
        (goal_text, goal),
        (readme_text, readme),
        (evidence_ledger_text, evidence_ledger),
    ):
        require(text, x11_implementation_checkpoint, source)
    require(evidence_ledger_text, "IOE-X11 Single-Owner TCP Server", evidence_ledger)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
        (assessment_text, assessment),
        (plan_text, plan),
        (goal_text, goal),
        (readme_text, readme),
        (evidence_ledger_text, evidence_ledger),
    ):
        require(text, x12_implementation_checkpoint, source)
    require(evidence_ledger_text, "IOE-X12 Multi-Owner TCP Server", evidence_ledger)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
        (assessment_text, assessment),
        (plan_text, plan),
        (goal_text, goal),
        (readme_text, readme),
        (evidence_ledger_text, evidence_ledger),
    ):
        require(text, x13_implementation_checkpoint, source)
    require(evidence_ledger_text, "IOE-X13 Active TCP Client", evidence_ledger)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
        (assessment_text, assessment),
        (plan_text, plan),
        (goal_text, goal),
        (readme_text, readme),
        (evidence_ledger_text, evidence_ledger),
    ):
        require(text, x14_implementation_checkpoint, source)
    require(evidence_ledger_text, "IOE-X14 Cross-Backend TCP Semantics", evidence_ledger)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
        (assessment_text, assessment),
        (plan_text, plan),
        (goal_text, goal),
        (readme_text, readme),
        (evidence_ledger_text, evidence_ledger),
    ):
        require(text, "M6/IOE-X15", source)
        require(text, x15_implementation_checkpoint, source)
        require(text, "M7", source)
    require(evidence_ledger_text, "IOE-X15 Experimental Installation Surface", evidence_ledger)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
        (assessment_text, assessment),
        (plan_text, plan),
        (goal_text, goal),
        (readme_text, readme),
        (evidence_ledger_text, evidence_ledger),
        (m7_readiness_text, m7_readiness),
    ):
        require(text, m7_readiness_checkpoint, source)
        require(text, "DEFER", source)
        require(text, "NO-PROMOTION", source)
    require(evidence_ledger_text, "M7-G0 External Lua / Typed-RPC Readiness", evidence_ledger)
    require(evidence_ledger_text, "M7-G0c External Adapter Closure", evidence_ledger)
    gateway_checkpoint = "0a8fe1e43cb11ac32daa8f9266d3b84924736e67"
    gateway_m4_closure = "d03cacd5aead885fc61a419c71d5c32a060cb700"
    gateway_m7_governance = "92a26072c3300275edc9d069a59fc17913c7614c"
    independent_consumer_checkpoint = "b5254165389d762c3f3c63568c24ffab448fc501"
    require(m7_readiness_text, "external implementation: `DEFER`", m7_readiness)
    require(m7_readiness_text, "external implementation: `RESUME`", m7_readiness)
    require(
        m7_readiness_text,
        "shared GameNet RPC promotion: `NO-PROMOTION`",
        m7_readiness,
    )
    require(m7_readiness_text, gateway_checkpoint, m7_readiness)
    require(m7_readiness_text, gateway_m4_closure, m7_readiness)
    require(m7_readiness_text, gateway_m7_governance, m7_readiness)
    require(m7_readiness_text, gateway_m7_implementation, m7_readiness)
    require(m7_readiness_text, gateway_m7_closure, m7_readiness)
    require(m7_readiness_text, independent_consumer_checkpoint, m7_readiness)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
        (assessment_text, assessment),
        (plan_text, plan),
        (goal_text, goal),
        (readme_text, readme),
        (evidence_ledger_text, evidence_ledger),
    ):
        require(text, gateway_m4_closure[:7], source)
        require(text, gateway_m7_governance[:7], source)
        require(text, gateway_m7_implementation[:7], source)
        require(text, gateway_m7_closure[:7], source)
        require(text, "NO-PROMOTION", source)
        require(text, "M8", source)
    require(m7_readiness_text, "external adapter validation: `COMPLETE`", m7_readiness)
    require(m7_readiness_text, "passed 11/11", m7_readiness)
    require(m7_readiness_text, "passed 20/20 each on both platforms", m7_readiness)
    require(m7_readiness_text, "passed 100,000", m7_readiness)
    require(m7_readiness_text, "passed 8/8", m7_readiness)
    for comparison_term in (
        "Wire",
        "Deadline",
        "Transport",
        "Request state",
        "Terminal behavior",
        "executed, divergent",
    ):
        require(m7_readiness_text, comparison_term, m7_readiness)
    deferred_catalog = intents_index_text.split("## Deferred Intent Catalog", 1)[1].split(
        "## Legacy Intent Catalog", 1
    )[0]
    require(evidence_ledger_text, "M8-G0 Async / Coroutine Promotion Audit", evidence_ledger)
    require(m8_readiness_text, m8_core_baseline, m8_readiness)
    require(m8_readiness_text, gateway_m7_implementation, m8_readiness)
    require(m8_readiness_text, gateway_m7_closure, m8_readiness)
    require(m8_readiness_text, independent_consumer_checkpoint, m8_readiness)
    require(m8_readiness_text, "passed the directly relevant contracts 3/3", m8_readiness)
    require(m8_readiness_text, "contract-fallback set passed 9/9", m8_readiness)
    require(m8_readiness_text, "shared async/coroutine promotion: `NO-PROMOTION`", m8_readiness)
    require(m8_readiness_text, "Result carrier", m8_readiness)
    require(m8_readiness_text, "Frame ownership", m8_readiness)
    require(m8_readiness_text, "TCP awaiters", m8_readiness)
    require(m8_readiness_text, "Composition", m8_readiness)
    require(m8_readiness_text, "advances to M9", m8_readiness)
    for name, path in async_intents.items():
        require(async_intent_texts[name], "status: deferred", path)
        require(deferred_catalog, f"- `intents/modules/{name}.intent.md`", intents_index)
        require(m8_readiness_text, f"`{name}`", m8_readiness)
    for text, source in (
        (status_text, migration_status),
        (roadmap_text, roadmap),
        (assessment_text, assessment),
        (plan_text, plan),
        (goal_text, goal),
        (readme_text, readme),
        (evidence_ledger_text, evidence_ledger),
    ):
        require(text, m8_core_baseline[:7], source)
        require(text, gateway_m7_closure[:7], source)
        require(text, independent_consumer_checkpoint[:7], source)
        require(text, "NO-PROMOTION", source)
        require(text, "M9", source)
    require(m7_readiness_text, "connection EventLoop owner", m7_readiness)
    require(m7_readiness_text, "gateway logic/Lua cell owner", m7_readiness)
    require(m7_readiness_text, "callback re-entry", m7_readiness)
    require(m7_readiness_text, "typed bounded admission", m7_readiness)
    require(
        m7_readiness_text,
        "tests/cmake/test_migration_status_contract.py",
        m7_readiness,
    )
    require(rpc_intent_text, "status: deferred", rpc_intent)
    require(intents_index_text, "- `intents/modules/rpc.intent.md`", intents_index)
    require(deferred_catalog, "- `intents/modules/rpc.intent.md`", intents_index)
    installed_rpc_or_lua_headers = [
        path
        for path in (repo_root / "include" / "gamenet").rglob("*")
        if path.is_file()
        and (
            "rpc" in path.relative_to(repo_root).as_posix().lower()
            or "lua" in path.relative_to(repo_root).as_posix().lower()
        )
    ]
    assert not installed_rpc_or_lua_headers, (
        "M7 closure must not install RPC/Lua headers: "
        + ", ".join(str(path) for path in installed_rpc_or_lua_headers)
    )
    tracked_cmake_paths = [
        path
        for path in git(repo_root, "ls-files").splitlines()
        if Path(path).name == "CMakeLists.txt" or Path(path).suffix == ".cmake"
    ]
    tracked_cmake_text = "\n".join(
        (repo_root / path).read_text(encoding="utf-8") for path in tracked_cmake_paths
    )
    assert "GameNet::rpc" not in tracked_cmake_text
    assert "gamenet_rpc" not in tracked_cmake_text
    installed_coroutine_or_awaiter_headers = [
        path
        for path in (repo_root / "include" / "gamenet").rglob("*")
        if path.is_file()
        and any(
            term in path.relative_to(repo_root).as_posix().lower()
            for term in ("coroutine", "awaiter", "when_all", "when_any")
        )
    ]
    assert not installed_coroutine_or_awaiter_headers, (
        "M8 closure must not install coroutine/awaiter headers: "
        + ", ".join(str(path) for path in installed_coroutine_or_awaiter_headers)
    )
    assert "GameNet::coroutine" not in tracked_cmake_text
    assert "gamenet_coroutine" not in tracked_cmake_text
    require(
        roadmap_text,
        "The current inventory is 130 CTest tests: 8 unit, 108 contract, and 14",
        roadmap,
    )
    require(roadmap_text, "103 threading and 108 lifecycle labels", roadmap)
    require(
        status_text,
        "inventory is 130 configured CTest tests: 8 unit tests, 108 contract tests",
        migration_status,
    )
    require(status_text, "103 threading and 108 lifecycle labels", migration_status)
    require(plan_text, "默认测试基线为 130", plan)
    require(plan_text, "Linux experimental 基线为 140", plan)
    require(plan_text, "# game-net-core 完整后续执行计划：IOE-X10 至 v1.0", plan)
    require(plan_text, "长期方向：`goal.md`", plan)
    require(plan_text, "当前评估：`assessment.md`", plan)
    require(plan_text, "当前唯一治理前沿是 **M9", plan)
    assert plan_text.count("当前唯一治理前沿") == 1, (
        "plan must declare exactly one current governance front"
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
    require(plan_text, "M5：v0.4 Runtime 边界", plan)
    require(plan_text, "状态：**已关闭，第二次 `NO-PROMOTION`**", plan)
    require(plan_text, "M9 TLS/WebSocket/DNS 证据审查是下一治理前沿", plan)
    require(plan_text, "runtime_profile_load_selection_guide.md", plan)
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
    require(
        common_profile_review_text,
        "Disposition: second `NO-PROMOTION`",
        common_profile_review,
    )
    for candidate in (
        "existing `TransportEndpoint`",
        "typed bounded `LogicExecutor` admission",
        "waitable monotonic `RuntimeStopFuture`",
        "shard types",
        "cadence types",
    ):
        require(common_profile_review_text, candidate, common_profile_review)
    for forbidden_runtime_shape in (
        "UniversalGameServer",
        "RuntimeProfileFactory",
        "AnyTransportAnyLogicRuntime",
    ):
        require(
            common_profile_review_text,
            forbidden_runtime_shape,
            common_profile_review,
        )
    require(common_profile_review_text, "`0a8fe1e`", common_profile_review)
    require(common_profile_review_text, "`736a090`", common_profile_review)
    require(common_profile_review_text, "no empty v0.4 release", common_profile_review)
    assert not (repo_root / "include" / "gamenet" / "runtime_profile").exists(), (
        "M5 NO-PROMOTION must not create an installed Runtime Profile surface"
    )
    for profile in (
        "A `SingleLoopInlineEvent`",
        "B `MultiIoQueuedEvent`",
        "C `MultiIoDedicatedFixedTick`",
        "D `MultiIoShardedHybrid`",
    ):
        require(profile_load_guide_text, profile, profile_load_guide)
    for workload_dimension in (
        "Connections",
        "Packet frequency and burst shape",
        "Logic cost",
        "Tick and cadence",
        "Handoff",
        "Broadcast",
        "Backpressure",
    ):
        require(profile_load_guide_text, workload_dimension, profile_load_guide)
    require(profile_load_guide_text, "provisional and non-installed", profile_load_guide)
    require(profile_load_guide_text, "paired ranking", profile_load_guide)
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
    require(assessment_text, "外部采用许可证阻塞已关闭", assessment)
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
    require(plan_text, "Linux experimental 基线为 140", plan)
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
