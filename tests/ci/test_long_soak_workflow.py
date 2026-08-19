from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


EXPECTED_THREADING_TESTS = 101
EXPECTED_PHASE4_SOAK_TESTS = 12
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
    assert workflow_text.count("fetch-tags: true") == 4, (
        "every full-history soak checkout must materialize annotated tags"
    )
    assert workflow_text.count("- name: Restore annotated candidate tag object") == 4
    assert workflow_text.count("if: github.ref_type == 'tag'") == 4
    assert workflow_text.count("git fetch --force --no-tags origin") == 4
    assert workflow_text.count(
        "+refs/tags/${{ github.ref_name }}:refs/tags/${{ github.ref_name }}"
    ) == 4
    require(workflow_text, "name: long-soak", workflow)
    require(workflow_text, "workflow_dispatch:", workflow)
    require(workflow_text, "          - ci", workflow)
    require(workflow_text, "          - candidate-waiver", workflow)
    require(workflow_text, "          - release-waiver", workflow)
    require(workflow_text, "      endurance_waiver_reason:", workflow)
    require(workflow_text, 'default: "50"', workflow)
    require(workflow_text, 'default: "60"', workflow)
    require(workflow_text, "      ci_artifact_policy:", workflow)
    require(workflow_text, "Artifact upload policy for the self-hosted ci matrix", workflow)
    require(workflow_text, "          - best-effort", workflow)
    require(workflow_text, "          - required", workflow)
    require(workflow_text, "        default: best-effort", workflow)
    assert "\n  push:" not in workflow_text, "long-soak must not run on push"
    assert "\n  pull_request:" not in workflow_text, "long-soak must not run on pull_request"
    permissions = re.search(r"(?m)^permissions:\n(?P<body>(?:  [^\n]+\n)+)", workflow_text)
    assert permissions is not None, "long-soak workflow needs explicit permissions"
    assert permissions.group("body") == "  actions: read\n  contents: read\n", (
        "long-soak workflow needs read-only actions and contents permissions"
    )

    job = job_block(workflow_text, "linux-long-soak")
    require(job, "Linux long-soak threading contracts", workflow)
    require(
        job,
        "runs-on: [self-hosted, linux, x64, gamenet-endurance]",
        workflow,
    )
    require(job, "timeout-minutes: 90", workflow)
    require(job, "run: cmake --build build-long-soak --parallel 1", workflow)
    require(job, "-DGAMENET_BUILD_TESTING=ON", workflow)
    require(job, "-DGAMENET_ENABLE_TLS=OFF", workflow)
    require(job, "-DGAMENET_ENABLE_EXPERIMENTAL=OFF", workflow)
    require(job, "python3 tests/cmake/test_event_loop_contracts.py", workflow)
    require(job, "python3 tests/scope/test_intent_metadata.py", workflow)
    require(job, "python3 tests/scope/test_intent_semantics.py", workflow)
    require(job, "python3 tests/api/test_public_api_manifest.py", workflow)
    require(job, "python3 tests/ci/test_performance_regression.py", workflow)
    require(job, "python3 tests/ci/test_endurance_gate.py", workflow)
    require(job, "python3 tests/cmake/test_core_benchmark_contract.py", workflow)
    require(job, "python3 tests/cmake/test_phase4_benchmark_contract.py", workflow)
    require(job, "python3 tests/cmake/test_capacity_profile_contract.py", workflow)
    require(job, "python3 tests/cmake/test_packet_framer_fuzz_contract.py", workflow)
    require(job, "python3 tests/cmake/test_logger_thread_contract.py", workflow)
    require(job, "python3 tests/cmake/test_build_governance_contract.py", workflow)
    require(job, "python3 tests/cmake/test_tcp_connection_thread_contract.py", workflow)
    require(job, "python3 tests/ci/test_core_benchmark_workflow.py", workflow)
    require(job, "python3 tests/ci/test_phase4_benchmark_workflow.py", workflow)

    checkout = step_block(job, "Checkout")
    require(checkout, "uses: actions/checkout@v4", workflow)
    require(checkout, "fetch-depth: 0", workflow)
    require(checkout, "fetch-tags: true", workflow)

    tag_restore = step_block(job, "Restore annotated candidate tag object")
    require(tag_restore, "if: github.ref_type == 'tag'", workflow)
    require(tag_restore, "git fetch --force --no-tags origin", workflow)
    require(
        tag_restore,
        "+refs/tags/${{ github.ref_name }}:refs/tags/${{ github.ref_name }}",
        workflow,
    )

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
    runtime_profile_guard = "python3 tests/cmake/test_runtime_profile_contract.py"
    queued_profile_guard = "python3 tests/cmake/test_multi_io_queued_profile_contract.py"
    fixed_tick_profile_guard = (
        "python3 tests/cmake/test_dedicated_fixed_tick_profile_contract.py"
    )
    sharded_hybrid_profile_guard = (
        "python3 tests/cmake/test_sharded_hybrid_profile_contract.py"
    )
    require(guards, verifier, workflow)
    require(guards, semantic_guard, workflow)
    require(guards, runtime_profile_guard, workflow)
    require(guards, queued_profile_guard, workflow)
    require(guards, fixed_tick_profile_guard, workflow)
    require(guards, sharded_hybrid_profile_guard, workflow)
    assert guards.index(verifier) < guards.index(semantic_guard), (
        "long-soak must verify immutable migration provenance before intent semantics"
    )
    assert job.index(source_checkout) < job.index(guards), (
        "long-soak must checkout migration provenance before repository guards"
    )
    assert job.index(tag_restore) < job.index(source_checkout), (
        "long-soak must restore the annotated tag before provenance checkout"
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
    require(inventory, "--expected-total 128", workflow)
    require(inventory, f"--expect-label threading={EXPECTED_THREADING_TESTS}", workflow)
    require(inventory, "--expect-label game_pipeline=7", workflow)
    require(inventory, "--expect-label broadcast=5", workflow)
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
        "python3 tools/verify_ctest_inventory.py --test-dir build-long-soak --expected-total 128",
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
    assert "continue-on-error:" not in upload, (
        "repeat-soak retained evidence upload must remain strict"
    )
    for producer_step in (inventory, threading_repeat, phase4_repeat, repeat_evidence):
        assert job.index(producer_step) < job.index(manifest), (
            "long-soak manifest must be written only after inventory, repeats, and "
            "structured repeat verification"
        )
    assert job.index(repeat_evidence) < job.index(manifest) < job.index(upload), (
        "long-soak manifest must hash verified repeat evidence before artifact upload"
    )

    self_hosted_ci = job_block(workflow_text, "linux-self-hosted-ci")
    require(self_hosted_ci, "if: inputs.mode == 'ci'", workflow)
    require(
        self_hosted_ci,
        "runs-on: [self-hosted, linux, x64, gamenet-endurance]",
        workflow,
    )
    require(self_hosted_ci, "fail-fast: false", workflow)
    require(self_hosted_ci, "max-parallel: 1", workflow)
    for profile in ("debug", "release", "asan-ubsan", "tsan"):
        require(self_hosted_ci, f"profile: {profile}", workflow)
    require(
        self_hosted_ci,
        "sanitizer_flag: -DGAMENET_ENABLE_ASAN_UBSAN=ON",
        workflow,
    )
    require(
        self_hosted_ci,
        "sanitizer_flag: -DGAMENET_ENABLE_TSAN=ON",
        workflow,
    )
    require(self_hosted_ci, "ctest_label: threading", workflow)
    require(
        self_hosted_ci,
        'run: cmake --build "${GAMENET_BUILD_DIR}" --parallel 1',
        workflow,
    )
    require(
        self_hosted_ci,
        'cmake --build "${GAMENET_CONSUMER_DIR}" --parallel 1',
        workflow,
    )
    require(self_hosted_ci, 'test "$(git rev-parse HEAD)" = "${GITHUB_SHA}"', workflow)
    self_hosted_tag_restore = step_block(
        self_hosted_ci, "Restore annotated candidate tag object"
    )
    require(self_hosted_tag_restore, "if: github.ref_type == 'tag'", workflow)
    require(
        self_hosted_tag_restore, "git fetch --force --no-tags origin", workflow
    )
    self_hosted_sanitizer_preflight = step_block(
        self_hosted_ci, "Verify ASan/UBSan runner environment"
    )
    require(
        self_hosted_sanitizer_preflight,
        "if: matrix.profile == 'asan-ubsan'",
        workflow,
    )
    require(self_hosted_sanitizer_preflight, '"/proc/${BASHPID}/status"', workflow)
    require(
        self_hosted_sanitizer_preflight,
        "/proc/sys/kernel/yama/ptrace_scope",
        workflow,
    )
    require(
        self_hosted_sanitizer_preflight,
        "tracer_pid=${tracer_pid}",
        workflow,
    )
    require(
        self_hosted_sanitizer_preflight,
        "ci-evidence/asan-ubsan-runner.txt",
        workflow,
    )
    require(
        self_hosted_sanitizer_preflight,
        "LeakSanitizer cannot run while the job shell is traced",
        workflow,
    )
    require(
        self_hosted_sanitizer_preflight,
        '[[ "${ptrace_scope}" =~ ^[0-9]+$ ]] && (( ptrace_scope != 0 ))',
        workflow,
    )
    require(
        self_hosted_sanitizer_preflight,
        "LeakSanitizer requires kernel.yama.ptrace_scope=0",
        workflow,
    )
    assert self_hosted_ci.index(self_hosted_sanitizer_preflight) < self_hosted_ci.index(
        "      - name: Check repository guards"
    ), "ASan/UBSan ptrace preflight must run before repository guards and the build"
    require(self_hosted_ci, "--expected-total 128", workflow)
    require(self_hosted_ci, "inventory+=(--expect-label threading=101)", workflow)
    require(self_hosted_ci, 'test_command+=(-L "${GAMENET_CTEST_LABEL}")', workflow)
    require(self_hosted_ci, "if: matrix.install_consumer", workflow)
    require(self_hosted_ci, "--expected-total 2", workflow)
    require(self_hosted_ci, "python3 tools/compare_public_api_manifest.py", workflow)
    require(
        self_hosted_ci,
        "--compatibility-baseline api/baselines/v0.3.0-perf-r1-reviewed.json",
        workflow,
    )
    require(self_hosted_ci, "--fail-on-compatibility-decision", workflow)
    require(self_hosted_ci, "--fail-on-stable-surface-review", workflow)
    require(
        self_hosted_ci,
        "--compatibility-output ci-evidence/public-api-compatibility-diff.json",
        workflow,
    )
    require(
        self_hosted_ci,
        "python3 tests/cmake/test_build_governance_contract.py",
        workflow,
    )

    self_hosted_manifest = step_block(
        self_hosted_ci, "Write self-hosted CI evidence manifest"
    )
    require(self_hosted_manifest, "if: always()", workflow)
    require(self_hosted_manifest, "python3 tools/write_ci_evidence.py", workflow)
    require(self_hosted_manifest, "--require-canonical-artifact-name", workflow)
    require(
        self_hosted_manifest,
        '--artifact-name "linux-self-hosted-${GAMENET_PROFILE}-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}"',
        workflow,
    )
    require(
        self_hosted_manifest,
        'GAMENET_CI_CANDIDATE_SHA: "${{ github.sha }}"',
        workflow,
    )
    require(
        self_hosted_manifest,
        'GAMENET_CI_STATUS: "${{ job.status }}"',
        workflow,
    )

    self_hosted_upload = step_block(
        self_hosted_ci, "Upload self-hosted CI evidence"
    )
    require(self_hosted_upload, "if: always()", workflow)
    require(self_hosted_upload, "id: self-hosted-ci-evidence-upload", workflow)
    require(
        self_hosted_upload,
        "continue-on-error: ${{ inputs.ci_artifact_policy == 'best-effort' }}",
        workflow,
    )
    require(self_hosted_upload, "uses: actions/upload-artifact@v4", workflow)
    require(
        self_hosted_upload,
        "name: linux-self-hosted-${{ matrix.profile }}-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}",
        workflow,
    )
    require(self_hosted_upload, "if-no-files-found: error", workflow)
    require(self_hosted_upload, "retention-days: 90", workflow)
    self_hosted_upload_report = step_block(
        self_hosted_ci, "Report self-hosted CI artifact upload"
    )
    require(self_hosted_upload_report, "if: always()", workflow)
    require(
        self_hosted_upload_report,
        "GAMENET_ARTIFACT_POLICY: ${{ inputs.ci_artifact_policy }}",
        workflow,
    )
    require(
        self_hosted_upload_report,
        "GAMENET_ARTIFACT_OUTCOME: ${{ steps.self-hosted-ci-evidence-upload.outcome }}",
        workflow,
    )
    require(
        self_hosted_upload_report,
        "The job result follows all preceding gates; no retained artifact may be claimed.",
        workflow,
    )
    assert self_hosted_ci.index(self_hosted_upload) < self_hosted_ci.index(
        self_hosted_upload_report
    ), "self-hosted artifact outcome must be reported after the upload attempt"

    production_job = job_block(workflow_text, "linux-production-endurance")
    require(
        production_job,
        "runs-on: [self-hosted, linux, x64, gamenet-endurance]",
        workflow,
    )

    production_checkout = step_block(
        production_job, "Checkout frozen candidate"
    )
    require(production_checkout, "uses: actions/checkout@v4", workflow)
    require(production_checkout, "fetch-depth: 0", workflow)
    require(production_checkout, "fetch-tags: true", workflow)
    require(production_checkout, "persist-credentials: false", workflow)

    production_tag_restore = step_block(
        production_job, "Restore annotated candidate tag object"
    )
    require(production_tag_restore, "if: github.ref_type == 'tag'", workflow)
    require(
        production_tag_restore, "git fetch --force --no-tags origin", workflow
    )

    production_source_checkout = step_block(
        production_job, "Checkout migration provenance source"
    )
    require(production_source_checkout, "uses: actions/checkout@v4", workflow)
    require(production_source_checkout, f"repository: {SOURCE_REPOSITORY}", workflow)
    require(production_source_checkout, f"ref: {SOURCE_COMMIT}", workflow)
    require(production_source_checkout, "path: mini_trantor", workflow)
    require(production_source_checkout, "persist-credentials: false", workflow)
    assert production_job.count("- name: Checkout migration provenance source") == 1

    production_guards = step_block(production_job, "Check repository guards")
    require(production_guards, verifier, workflow)
    require(production_guards, semantic_guard, workflow)
    require(production_guards, runtime_profile_guard, workflow)
    require(production_guards, queued_profile_guard, workflow)
    assert production_guards.index(verifier) < production_guards.index(semantic_guard), (
        "production endurance must verify immutable migration provenance before "
        "intent semantics"
    )
    assert production_job.index(production_source_checkout) < production_job.index(
        production_guards
    ), "production endurance must checkout migration provenance before repository guards"
    assert production_job.index(production_tag_restore) < production_job.index(
        production_source_checkout
    ), "production endurance must restore annotated tag before provenance checkout"

    production_build = step_block(production_job, "Build Release endurance target")
    require(
        production_build,
        "run: cmake --build build-production-endurance --parallel 1",
        workflow,
    )

    production_artifact_name = (
        "production-endurance-${{ inputs.mode }}-${{ github.job }}-${{ github.sha }}-"
        "${{ github.run_id }}-${{ github.run_attempt }}"
    )
    production_manifest = step_block(
        production_job, "Write endurance evidence manifest"
    )
    require(
        production_manifest,
        f"--artifact-name '{production_artifact_name}'",
        workflow,
    )
    require(production_manifest, "--require-canonical-artifact-name", workflow)
    require(
        production_manifest,
        "--command 'cmake --build build-production-endurance --parallel 1'",
        workflow,
    )

    production_upload = step_block(
        production_job, "Upload production endurance evidence"
    )
    require(production_upload, f"name: {production_artifact_name}", workflow)
    assert "continue-on-error:" not in production_upload, (
        "production endurance retained evidence upload must remain strict"
    )

    candidate_download = step_block(
        production_job, "Download retained candidate-24h evidence"
    )
    require(
        candidate_download,
        "pattern: production-endurance-candidate-24h-${{ github.job }}-"
        "${{ github.sha }}-${{ inputs.candidate_run_id }}-"
        "${{ inputs.candidate_run_attempt }}",
        workflow,
    )
    capacity_download = step_block(
        production_job, "Download exact paired capacity evidence"
    )
    require(
        capacity_download,
        "capacity-gate-pair-${{ inputs.mode == 'candidate-24h' && "
        "'candidate-10k' || 'dedicated-100k' }}-${{ github.sha }}-"
        "${{ inputs.capacity_run_id }}-${{ inputs.capacity_run_attempt }}",
        workflow,
    )
    promotion_step = step_block(
        production_job, "Verify production promotion evidence"
    )
    require(
        promotion_step,
        "tools/verify_production_promotion_evidence.py",
        workflow,
    )
    require(
        promotion_step,
        "--candidate-endurance-run-attempt",
        workflow,
    )
    capacity_revalidation = step_block(
        production_job, "Revalidate exact paired capacity source"
    )
    require(
        capacity_revalidation,
        "--retained-pair-manifest capacity-evidence/pair-manifest.json",
        workflow,
    )
    endurance_run = step_block(
        production_job, "Run uninterrupted production endurance"
    )
    assert production_job.index(capacity_revalidation) < production_job.index(
        endurance_run
    ), "capacity evidence must fail before the expensive endurance process starts"

    waiver_job = job_block(workflow_text, "production-endurance-waiver")
    require(
        waiver_job,
        "if: inputs.mode == 'candidate-waiver' || inputs.mode == 'release-waiver'",
        workflow,
    )
    require(waiver_job, "runs-on: ubuntu-24.04", workflow)
    assert "tools/run_endurance_gate.py" not in waiver_job, (
        "waiver job must not launch a long-duration endurance process"
    )
    waiver_source_checkout = step_block(
        waiver_job, "Checkout migration provenance source"
    )
    waiver_guards = step_block(waiver_job, "Check repository guards")
    require(waiver_source_checkout, f"repository: {SOURCE_REPOSITORY}", workflow)
    require(waiver_source_checkout, f"ref: {SOURCE_COMMIT}", workflow)
    require(waiver_guards, verifier, workflow)
    require(waiver_guards, semantic_guard, workflow)
    assert waiver_guards.index(verifier) < waiver_guards.index(semantic_guard)
    assert waiver_job.index(waiver_source_checkout) < waiver_job.index(waiver_guards)

    waiver_validation = step_block(
        waiver_job, "Validate production endurance waiver inputs"
    )
    require(waiver_validation, "endurance waiver must not name endurance evidence", workflow)
    require(waiver_validation, "endurance waiver requires exact capacity run/attempt", workflow)
    require(waiver_validation, "12-500 character single line", workflow)
    waiver_download = step_block(
        waiver_job, "Download exact paired waiver capacity evidence"
    )
    require(
        waiver_download,
        "inputs.mode == 'candidate-waiver' && 'candidate-10k' || 'dedicated-100k'",
        workflow,
    )
    waiver_revalidation = step_block(
        waiver_job, "Revalidate exact paired waiver capacity source"
    )
    require(waiver_revalidation, "--retained-pair-manifest capacity-evidence/pair-manifest.json", workflow)
    waiver_record = step_block(
        waiver_job, "Record owner-approved production endurance waiver"
    )
    require(waiver_record, "--waive-endurance", workflow)
    require(waiver_record, "--waiver-reason", workflow)
    require(waiver_record, "--waiver-approved-by '${{ github.actor }}'", workflow)
    require(
        waiver_record,
        "inputs.mode == 'candidate-waiver' && 'candidate' || 'release'",
        workflow,
    )
    assert waiver_job.index(waiver_revalidation) < waiver_job.index(waiver_record)
    waiver_manifest = step_block(
        waiver_job, "Write production endurance waiver manifest"
    )
    waiver_artifact_name = (
        "production-endurance-${{ inputs.mode }}-${{ github.job }}-${{ github.sha }}-"
        "${{ github.run_id }}-${{ github.run_attempt }}"
    )
    require(waiver_manifest, f"--artifact-name '{waiver_artifact_name}'", workflow)
    require(waiver_manifest, "--require-canonical-artifact-name", workflow)
    waiver_upload = step_block(
        waiver_job, "Upload production endurance waiver evidence"
    )
    require(waiver_upload, f"name: {waiver_artifact_name}", workflow)
    require(waiver_upload, "if-no-files-found: error", workflow)

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
    require(ci_docs_text, "101 threading-labeled tests", ci_docs)
    require(ci_docs_text, "12 Pipeline/Broadcast tests", ci_docs)
    require(ci_docs_text, "`ci_artifact_policy`", ci_docs)
    require(ci_docs_text, "defaults to `best-effort`", ci_docs)
    require(ci_docs_text, "Selecting `required` restores the strict upload gate", ci_docs)
    require(
        ci_docs_text,
        "An\naccount-level lock that prevents GitHub from scheduling the workflow at all\n"
        "cannot be bypassed by repository YAML",
        ci_docs,
    )
    require(ci_docs_text, "does not provide retained artifact\nevidence", ci_docs)
    require(
        ci_docs_text,
        "Repeat-soak and actual 24/72-hour production-endurance uploads",
        ci_docs,
    )
    require(ci_docs_text, "`candidate-waiver` and `release-waiver`", ci_docs)
    require(ci_docs_text, "kernel.yama.ptrace_scope=0", ci_docs)
    require(ci_docs_text, "/etc/sysctl.d/99-gamenet-lsan.conf", ci_docs)
    require(ci_docs_text, "ci-evidence/asan-ubsan-runner.txt", ci_docs)
    require(ci_docs_text, "dedicated trusted endurance runner", ci_docs)
    require(ci_docs_text, "ASAN_OPTIONS=detect_leaks=0", ci_docs)
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
