from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


EXPECTED_THREADING_TESTS = 61
EXPECTED_PHASE4_SOAK_TESTS = 8
SOURCE_REPOSITORY = "YanqingXu/mini_trantor"
SOURCE_COMMIT = "3eba368475a68f677aae920d4f299b155db23d57"


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing long-soak workflow fragment in {source}: {needle}"


def job_block(workflow: str, job_name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(job_name)}:\n(?P<body>.*?)(?=^  [a-z0-9_-]+:\n|\Z)",
        workflow,
    )
    assert match is not None, f"missing long-soak workflow job: {job_name}"
    return match.group(0)


def step_block(job: str, step_name: str) -> str:
    match = re.search(
        rf"(?ms)^      - name: {re.escape(step_name)}\n(?P<body>.*?)(?=^      - name: |\Z)",
        job,
    )
    assert match is not None, f"missing long-soak workflow step: {step_name}"
    return match.group(0)


def verify_repeat_evidence_tool(repo_root: Path) -> None:
    verifier = repo_root / "tools" / "verify_ctest_repeat_evidence.py"
    assert verifier.is_file(), f"missing structured repeat verifier: {verifier}"
    with tempfile.TemporaryDirectory(prefix="gamenet-repeat-evidence-") as directory:
        root = Path(directory)
        inventory = root / "ctest-inventory.json"
        log = root / "repeat.log"
        output = root / "repeat-evidence.json"
        inventory.write_text(
            json.dumps(
                {
                    "schema": "gamenet.ctest_inventory.v1",
                    "tests": [
                        {"name": "contract.one", "labels": ["threading"]},
                        {"name": "contract.two", "labels": ["threading", "lifecycle"]},
                        {"name": "contract.other", "labels": ["other"]},
                    ],
                }
            ),
            encoding="utf-8",
        )

        def write_log(second_count: int, *, failed: bool = False) -> None:
            lines = ["Test project fixture"]
            for _ in range(2):
                lines.append("    Test #1: contract.one ..........   Passed    0.01 sec")
            for index in range(second_count):
                status = "***Failed" if failed and index == 0 else "Passed"
                lines.append(f"    Test #2: contract.two ..........   {status}    0.01 sec")
            lines.extend(
                [
                    "100% tests passed, 0 tests failed out of 2",
                    "Total Test time (real) = 0.04 sec",
                ]
            )
            log.write_text("\n".join(lines) + "\n", encoding="utf-8")

        command = [
            sys.executable,
            str(verifier),
            "--inventory",
            str(inventory),
            "--log",
            str(log),
            "--selection-label",
            "threading",
            "--expected-selected",
            "2",
            "--repeat",
            "2",
            "--timeout-seconds",
            "60",
            "--command",
            "ctest --repeat until-fail:2",
            "--output",
            str(output),
        ]

        write_log(2)
        positive = subprocess.run(command, capture_output=True, text=True, check=False)
        assert positive.returncode == 0, positive.stderr
        evidence = json.loads(output.read_text(encoding="utf-8"))
        assert evidence["schema"] == "gamenet.ctest_repeat_evidence.v1"
        assert evidence["expected_executions"] == 4
        assert evidence["actual_executions"] == 4
        assert [test["executions"] for test in evidence["tests"]] == [2, 2]

        write_log(1)
        missing = subprocess.run(command, capture_output=True, text=True, check=False)
        assert missing.returncode != 0
        assert "repeat execution count mismatch" in missing.stderr

        write_log(2, failed=True)
        failure = subprocess.run(command, capture_output=True, text=True, check=False)
        assert failure.returncode != 0
        assert "non-passing result lines" in failure.stderr


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    workflow = repo_root / ".github" / "workflows" / "long-soak.yml"
    ci_workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    migration_status = repo_root / "docs" / "migration_status.md"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"

    verify_repeat_evidence_tool(repo_root)

    assert workflow.exists(), f"missing non-default long-soak workflow: {workflow}"

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "name: long-soak", workflow)
    require(workflow_text, "workflow_dispatch:", workflow)
    require(workflow_text, 'default: "50"', workflow)
    require(workflow_text, 'default: "60"', workflow)
    assert "\n  push:" not in workflow_text, "long-soak must not run on push"
    assert "\n  pull_request:" not in workflow_text, "long-soak must not run on pull_request"
    permissions = re.search(r"(?m)^permissions:\n(?P<body>(?:  [^\n]+\n)+)", workflow_text)
    assert permissions is not None, "long-soak workflow needs explicit permissions"
    assert permissions.group("body") == "  contents: read\n", (
        "long-soak workflow must retain contents: read as its only repository permission"
    )

    job = job_block(workflow_text, "linux-long-soak")
    require(job, "Linux long-soak threading contracts", workflow)
    require(job, "runs-on: ubuntu-24.04", workflow)
    require(job, "timeout-minutes: 90", workflow)
    require(job, "-DGAMENET_BUILD_TESTING=ON", workflow)
    require(job, "-DGAMENET_ENABLE_TLS=OFF", workflow)
    require(job, "-DGAMENET_ENABLE_EXPERIMENTAL=OFF", workflow)
    require(job, "python3 tests/cmake/test_event_loop_contracts.py", workflow)
    require(job, "python3 tests/scope/test_intent_metadata.py", workflow)
    require(job, "python3 tests/scope/test_intent_semantics.py", workflow)
    require(job, "python3 tests/cmake/test_core_benchmark_contract.py", workflow)
    require(job, "python3 tests/cmake/test_phase4_benchmark_contract.py", workflow)
    require(job, "python3 tests/cmake/test_packet_framer_fuzz_contract.py", workflow)
    require(job, "python3 tests/cmake/test_logger_thread_contract.py", workflow)
    require(job, "python3 tests/cmake/test_tcp_connection_thread_contract.py", workflow)
    require(job, "python3 tests/ci/test_core_benchmark_workflow.py", workflow)
    require(job, "python3 tests/ci/test_phase4_benchmark_workflow.py", workflow)

    source_checkout = step_block(job, "Checkout migration provenance source")
    require(source_checkout, "uses: actions/checkout@v4", workflow)
    require(source_checkout, f"repository: {SOURCE_REPOSITORY}", workflow)
    require(source_checkout, f"ref: {SOURCE_COMMIT}", workflow)
    require(source_checkout, "path: mini_trantor", workflow)
    require(source_checkout, "persist-credentials: false", workflow)
    assert job.count("- name: Checkout migration provenance source") == 1

    guards = step_block(job, "Check repository guards")
    verifier = "python3 tools/verify_migration_provenance.py"
    semantic_guard = "python3 tests/scope/test_intent_semantics.py"
    require(guards, verifier, workflow)
    require(guards, semantic_guard, workflow)
    assert guards.index(verifier) < guards.index(semantic_guard), (
        "long-soak must verify immutable migration provenance before intent semantics"
    )
    assert job.index(source_checkout) < job.index(guards), (
        "long-soak must checkout migration provenance before repository guards"
    )

    validation = step_block(job, "Validate long-soak inputs")
    require(validation, 'GAMENET_SOAK_REPEAT_INPUT: "${{ inputs.repeat }}"', workflow)
    require(validation, 'GAMENET_SOAK_TIMEOUT_INPUT: "${{ inputs.timeout_seconds }}"', workflow)
    require(validation, '"GAMENET_SOAK_REPEAT", 50', workflow)
    require(validation, '"GAMENET_SOAK_TIMEOUT_SECONDS", 60', workflow)
    require(validation, 're.fullmatch(r"[0-9]+", raw)', workflow)
    require(validation, "if value < minimum:", workflow)
    require(validation, 'os.environ["GITHUB_ENV"]', workflow)
    assert workflow_text.count("${{ inputs.repeat }}") == 1, (
        "raw repeat input may only enter the quoted validation-step environment"
    )
    assert workflow_text.count("${{ inputs.timeout_seconds }}") == 1, (
        "raw timeout input may only enter the quoted validation-step environment"
    )

    inventory = step_block(job, "Verify long-soak test inventory")
    require(inventory, "set -euo pipefail", workflow)
    require(inventory, "python3 tools/verify_ctest_inventory.py", workflow)
    require(inventory, "--expected-total 85", workflow)
    require(inventory, f"--expect-label threading={EXPECTED_THREADING_TESTS}", workflow)
    require(inventory, "--expect-label game_pipeline=4", workflow)
    require(inventory, "--expect-label broadcast=4", workflow)
    require(inventory, "--output long-soak-evidence/ctest-inventory.json", workflow)

    threading_repeat = step_block(job, "Repeated threading CTest")
    require(threading_repeat, "set -euo pipefail", workflow)
    require(threading_repeat, "ctest --test-dir build-long-soak --output-on-failure", workflow)
    require(threading_repeat, "-L threading", workflow)
    require(threading_repeat, '--repeat "until-fail:${GAMENET_SOAK_REPEAT}"', workflow)
    require(threading_repeat, '--timeout "${GAMENET_SOAK_TIMEOUT_SECONDS}"', workflow)
    require(threading_repeat, "2>&1 | tee long-soak-evidence/threading-repeat.log", workflow)

    phase4_repeat = step_block(job, "Repeated Pipeline and Broadcast CTest")
    require(phase4_repeat, "set -euo pipefail", workflow)
    require(phase4_repeat, "ctest --test-dir build-long-soak --output-on-failure", workflow)
    require(phase4_repeat, '-L "game_pipeline|broadcast"', workflow)
    require(phase4_repeat, '--repeat "until-fail:${GAMENET_SOAK_REPEAT}"', workflow)
    require(phase4_repeat, '--timeout "${GAMENET_SOAK_TIMEOUT_SECONDS}"', workflow)
    require(phase4_repeat, "2>&1 | tee long-soak-evidence/phase4-repeat.log", workflow)

    repeat_evidence = step_block(job, "Verify repeated execution evidence")
    require(repeat_evidence, "set -euo pipefail", workflow)
    assert repeat_evidence.count("python3 tools/verify_ctest_repeat_evidence.py") == 2
    require(repeat_evidence, "--inventory long-soak-evidence/ctest-inventory.json", workflow)
    require(repeat_evidence, "--log long-soak-evidence/threading-repeat.log", workflow)
    require(repeat_evidence, "--selection-label threading", workflow)
    require(repeat_evidence, f"--expected-selected {EXPECTED_THREADING_TESTS}", workflow)
    require(repeat_evidence, "--log long-soak-evidence/phase4-repeat.log", workflow)
    require(repeat_evidence, "--selection-label game_pipeline", workflow)
    require(repeat_evidence, "--selection-label broadcast", workflow)
    require(repeat_evidence, f"--expected-selected {EXPECTED_PHASE4_SOAK_TESTS}", workflow)
    require(repeat_evidence, '--repeat "${GAMENET_SOAK_REPEAT}"', workflow)
    require(repeat_evidence, '--timeout-seconds "${GAMENET_SOAK_TIMEOUT_SECONDS}"', workflow)
    require(repeat_evidence, "--output long-soak-evidence/threading-repeat-evidence.json", workflow)
    require(repeat_evidence, "--output long-soak-evidence/phase4-repeat-evidence.json", workflow)

    manifest = step_block(job, "Write long-soak evidence manifest")
    require(manifest, "if: always()", workflow)
    require(manifest, "python3 tools/write_ci_evidence.py", workflow)
    require(manifest, "--evidence-root long-soak-evidence", workflow)
    require(manifest, "--output long-soak-evidence/manifest.json", workflow)
    require(manifest, "--require-canonical-artifact-name", workflow)
    require(
        manifest,
        "--artifact-name 'long-soak-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}'",
        workflow,
    )
    require(manifest, 'GAMENET_CI_CANDIDATE_SHA: "${{ github.sha }}"', workflow)
    require(manifest, 'GAMENET_CI_STATUS: "${{ job.status }}"', workflow)
    require(
        manifest,
        "python3 tools/verify_ctest_inventory.py --test-dir build-long-soak --expected-total 85",
        workflow,
    )
    assert "ctest --test-dir build-long-soak -N" not in manifest
    require(manifest, '--command "ctest --test-dir build-long-soak', workflow)
    require(manifest, "--repeat until-fail:${GAMENET_SOAK_REPEAT}", workflow)
    assert 'GAMENET_CI_STATUS: "success"' not in manifest, (
        "long-soak manifest status must reflect job.status rather than a hard-coded success"
    )

    upload = step_block(job, "Upload long-soak evidence")
    require(upload, "if: always()", workflow)
    require(upload, "uses: actions/upload-artifact@v4", workflow)
    require(
        upload,
        "name: long-soak-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}",
        workflow,
    )
    for evidence_path in (
        "long-soak-evidence/toolchain.txt",
        "long-soak-evidence/ctest-inventory.json",
        "long-soak-evidence/threading-repeat.log",
        "long-soak-evidence/phase4-repeat.log",
        "long-soak-evidence/threading-repeat-evidence.json",
        "long-soak-evidence/phase4-repeat-evidence.json",
        "long-soak-evidence/manifest.json",
    ):
        require(upload, evidence_path, workflow)
    require(upload, "if-no-files-found: error", workflow)
    require(upload, "retention-days: 90", workflow)
    for producer_step in (inventory, threading_repeat, phase4_repeat, repeat_evidence):
        assert job.index(producer_step) < job.index(manifest), (
            "long-soak manifest must be written only after inventory, repeats, and "
            "structured repeat verification"
        )
    assert job.index(repeat_evidence) < job.index(manifest) < job.index(upload), (
        "long-soak manifest must hash verified repeat evidence before artifact upload"
    )

    cmake_calls = re.findall(
        r"^add_gamenet_(?:component_)?test\(([^\n]*)\)$",
        tests_cmake.read_text(encoding="utf-8"),
        flags=re.MULTILINE,
    )
    threading_count = sum("threading" in call.split() for call in cmake_calls)
    phase4_soak_count = sum(
        len(call.split()) >= 2 and call.split()[1] in {"game_pipeline", "broadcast"}
        for call in cmake_calls
    )
    assert threading_count == EXPECTED_THREADING_TESTS, (
        f"threading inventory changed: expected {EXPECTED_THREADING_TESTS}, got {threading_count}; "
        "update the long-soak inventory gate and evidence documentation intentionally"
    )
    assert phase4_soak_count == EXPECTED_PHASE4_SOAK_TESTS, (
        f"Pipeline/Broadcast soak inventory changed: expected {EXPECTED_PHASE4_SOAK_TESTS}, "
        f"got {phase4_soak_count}; update the dedicated repeat gate intentionally"
    )

    ci_workflow_text = ci_workflow.read_text(encoding="utf-8")
    require(ci_workflow_text, "python3 tests/ci/test_long_soak_workflow.py", ci_workflow)
    require(ci_workflow_text, "python tests/ci/test_long_soak_workflow.py", ci_workflow)

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, "test_long_soak_workflow.py", ci_docs)
    require(ci_docs_text, "tools/verify_migration_provenance.py", ci_docs)
    require(ci_docs_text, SOURCE_REPOSITORY, ci_docs)
    require(ci_docs_text, SOURCE_COMMIT, ci_docs)
    require(ci_docs_text, "Git object", ci_docs)
    require(ci_docs_text, "Long-soak repository guards include `tests/cmake/test_event_loop_contracts.py`", ci_docs)
    require(ci_docs_text, "long-soak", ci_docs)
    require(ci_docs_text, "ctest --test-dir build-long-soak --output-on-failure", ci_docs)
    require(ci_docs_text, "--repeat until-fail:", ci_docs)
    require(ci_docs_text, "defaults to repeat 50", ci_docs)
    require(ci_docs_text, "60-second per-test timeout", ci_docs)
    require(ci_docs_text, "61 threading-labeled tests", ci_docs)
    require(ci_docs_text, "8 Pipeline/Broadcast tests", ci_docs)
    require(ci_docs_text, "Phase 3.5 historical evidence: run `29077148022`", ci_docs)
    require(ci_docs_text, "job `86311227712`", ci_docs)
    require(ci_docs_text, "`a7fd77cbd2140041cebb3f900d5c609fafc2adad`", ci_docs)
    require(ci_docs_text, "repeat 50", ci_docs)
    require(ci_docs_text, "timeout 60 seconds", ci_docs)
    require(ci_docs_text, "46/46\nthreading-labeled tests passed", ci_docs)
    require(ci_docs_text, "1632.47 seconds", ci_docs)
    require(ci_docs_text, "28m27s", ci_docs)
    require(
        ci_docs_text,
        "ctest --test-dir build-long-soak --output-on-failure -L threading --repeat until-fail:50 --timeout 60",
        ci_docs,
    )
    require(ci_docs_text, "previous 43-test threading slice", ci_docs)
    require(ci_docs_text, "then-expanded 44-test threading slice", ci_docs)
    require(ci_docs_text, "then-present threading slice to 46 tests", ci_docs)
    require(ci_docs_text, "46-test threading\nslice across 5 repeats", ci_docs)

    migration_text = migration_status.read_text(encoding="utf-8")
    normalized_migration_text = " ".join(migration_text.split())
    require(migration_text, "non-default `long-soak` workflow", migration_status)
    require(migration_text, "long-soak repository guard parity includes the EventLoop contract guard", migration_status)
    require(migration_text, "`ctest --repeat until-fail`", migration_status)
    require(
        normalized_migration_text,
        "corresponding historical remote GitHub `long-soak` evidence is recorded",
        migration_status,
    )


if __name__ == "__main__":
    main()
