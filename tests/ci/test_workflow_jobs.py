from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


SOURCE_REPOSITORY = "YanqingXu/mini_trantor"
SOURCE_COMMIT = "3eba368475a68f677aae920d4f299b155db23d57"
EXPECTED_CTEST_TOTAL = 85
EXPECTED_THREADING_TOTAL = 61
ARTIFACT_NAME = (
    "ci-evidence-${{ github.job }}-${{ github.sha }}-"
    "${{ github.run_id }}-${{ github.run_attempt }}"
)
AGGREGATE_ARTIFACT_NAME = (
    "ci-evidence-set-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}"
)


@dataclass(frozen=True)
class JobContract:
    interpreter: str
    timeout_minutes: int
    test_dir: str
    config: str | None
    configure_step: str
    build_step: str
    test_step: str
    test_timeout: int
    junit_paths: tuple[str, ...]
    manifest_command_count: int
    package_step: str | None = None
    threading_inventory: bool = False
    fuzz: bool = False


JOB_CONTRACTS = {
    "linux-cmake": JobContract(
        "python3",
        30,
        "build",
        None,
        "Configure",
        "Build",
        "Test",
        60,
        ("ci-evidence/ctest-junit.xml", "ci-evidence/install-consumer-junit.xml"),
        9,
        package_step="Install and verify package consumer",
    ),
    "linux-asan-ubsan": JobContract(
        "python3",
        45,
        "build-asan",
        None,
        "Configure",
        "Build",
        "Test",
        60,
        ("ci-evidence/ctest-junit.xml",),
        11,
        fuzz=True,
    ),
    "linux-tsan": JobContract(
        "python3",
        45,
        "build-tsan",
        None,
        "Configure",
        "Build",
        "Test",
        60,
        ("ci-evidence/ctest-junit.xml",),
        4,
        threading_inventory=True,
    ),
    "linux-release": JobContract(
        "python3",
        30,
        "build-release",
        None,
        "Configure",
        "Build",
        "Test",
        60,
        ("ci-evidence/ctest-junit.xml",),
        4,
    ),
    "windows-msvc": JobContract(
        "python",
        45,
        "build-windows",
        "Debug",
        "Configure",
        "Build",
        "Test",
        10,
        ("ci-evidence/ctest-junit.xml", "ci-evidence/install-consumer-junit.xml"),
        9,
        package_step="Install and verify package consumer",
    ),
    "windows-msvc-release": JobContract(
        "python",
        45,
        "build-windows-release",
        "Release",
        "Configure Release",
        "Build Release",
        "Test Release",
        10,
        ("ci-evidence/ctest-junit.xml", "ci-evidence/install-consumer-junit.xml"),
        9,
        package_step="Install and verify Release package consumer",
    ),
}

PROVENANCE_JOBS = tuple(
    (job_name, contract.interpreter) for job_name, contract in JOB_CONTRACTS.items()
)


def require(text: str, needle: str) -> None:
    assert needle in text, f"missing workflow fragment: {needle}"


def job_block(workflow: str, job_name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(job_name)}:\n(?P<body>.*?)(?=^  [a-z0-9_-]+:\n|\Z)",
        workflow,
    )
    assert match is not None, f"missing workflow job: {job_name}"
    return match.group(0)


def step_block(job: str, step_name: str) -> str:
    match = re.search(
        rf"(?ms)^      - name: {re.escape(step_name)}\n(?P<body>.*?)(?=^      - name: |\Z)",
        job,
    )
    assert match is not None, f"missing workflow step: {step_name}"
    return match.group(0)


def verify_manifest_writer(repo_root: Path) -> None:
    writer = repo_root / "tools" / "write_ci_evidence.py"
    assert writer.is_file(), f"missing CI evidence writer: {writer}"
    checkout_sha = subprocess.run(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=repo_root,
        check=True,
        stdout=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    ).stdout.strip()
    command = "ctest --test-dir build --timeout 60 --output-junit ci-evidence/ctest-junit.xml"

    def run_writer(
        *,
        event_name: str,
        github_sha: str,
        candidate_sha: str,
        merge_sha: str,
        pr_head_sha: str | None,
        artifact_name: str,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, object] | None, bytes]:
        with tempfile.TemporaryDirectory(prefix="gamenet-ci-evidence-") as temporary:
            evidence_root = Path(temporary) / "evidence"
            evidence_root.mkdir()
            toolchain = evidence_root / "toolchain.txt"
            toolchain.write_bytes(b"cmake version fixture\n")
            output = evidence_root / "manifest.json"
            environment = os.environ.copy()
            environment.update(
                {
                    "GITHUB_SHA": github_sha,
                    "GITHUB_RUN_ID": "456",
                    "GITHUB_RUN_ATTEMPT": "3",
                    "GITHUB_JOB": "linux-cmake",
                    "GITHUB_WORKFLOW": "ci",
                    "GITHUB_REF": "refs/pull/7/merge" if event_name == "pull_request" else "refs/heads/main",
                    "GITHUB_EVENT_NAME": event_name,
                    "GITHUB_REPOSITORY": "YanqingXu/game-net-core",
                    "GAMENET_CI_CANDIDATE_SHA": candidate_sha,
                    "GAMENET_CI_MERGE_OR_CURRENT_SHA": merge_sha,
                    "GAMENET_CI_STATUS": "success",
                    "RUNNER_OS": "Windows",
                    "RUNNER_ARCH": "X64",
                }
            )
            if pr_head_sha is None:
                environment.pop("GAMENET_CI_PR_HEAD_SHA", None)
            else:
                environment["GAMENET_CI_PR_HEAD_SHA"] = pr_head_sha
            result = subprocess.run(
                [
                    sys.executable,
                    str(writer),
                    "--evidence-root",
                    str(evidence_root),
                    "--output",
                    str(output),
                    "--artifact-name",
                    artifact_name,
                    "--require-canonical-artifact-name",
                    "--command",
                    command,
                ],
                cwd=repo_root,
                env=environment,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            manifest = json.loads(output.read_text(encoding="utf-8")) if output.exists() else None
            return result, manifest, toolchain.read_bytes()

    candidate_sha = "1" * 40
    artifact_name = f"ci-evidence-linux-cmake-{checkout_sha}-456-3"
    result, manifest, toolchain_bytes = run_writer(
        event_name="pull_request",
        github_sha=checkout_sha,
        candidate_sha=candidate_sha,
        merge_sha=checkout_sha,
        pr_head_sha=candidate_sha,
        artifact_name=artifact_name,
    )
    assert result.returncode == 0, result.stderr
    assert manifest is not None
    assert manifest["schema"] == "gamenet.ci_evidence.v1"
    assert manifest["checkout_sha"] == checkout_sha
    assert manifest["candidate_sha"] == candidate_sha
    assert manifest["github_sha"] == checkout_sha
    assert manifest["pr_head_sha"] == candidate_sha
    assert manifest["merge_or_current_sha"] == checkout_sha
    assert manifest["run_id"] == "456"
    assert manifest["run_attempt"] == 3
    assert manifest["job"] == "linux-cmake"
    assert manifest["workflow"] == "ci"
    assert manifest["ref"] == "refs/pull/7/merge"
    assert manifest["status"] == "success"
    assert manifest["pre_upload_status"] == "success"
    assert manifest["status_scope"] == "job_before_evidence_upload"
    assert manifest["runner"] == {"os": "Windows", "arch": "X64"}
    assert manifest["artifact_name"] == artifact_name
    assert manifest["commands"] == [command]
    assert re.fullmatch(r"\d{4}-\d{2}-\d{2}", manifest["date_utc"])
    assert manifest["generated_at_utc"].endswith("Z")
    assert manifest["evidence_files"] == [
        {
            "path": "toolchain.txt",
            "bytes": len(toolchain_bytes),
            "sha256": hashlib.sha256(toolchain_bytes).hexdigest(),
        }
    ]

    result, push_manifest, _ = run_writer(
        event_name="push",
        github_sha=checkout_sha,
        candidate_sha=checkout_sha,
        merge_sha=checkout_sha,
        pr_head_sha=None,
        artifact_name=artifact_name,
    )
    assert result.returncode == 0, result.stderr
    assert push_manifest is not None and push_manifest["candidate_sha"] == checkout_sha

    negative_cases = (
        (
            "checkout SHA must equal GITHUB_SHA",
            dict(
                event_name="push",
                github_sha="2" * 40,
                candidate_sha="2" * 40,
                merge_sha="2" * 40,
                pr_head_sha=None,
                artifact_name=f"ci-evidence-linux-cmake-{'2' * 40}-456-3",
            ),
        ),
        (
            "merge/current SHA must equal GITHUB_SHA",
            dict(
                event_name="push",
                github_sha=checkout_sha,
                candidate_sha=checkout_sha,
                merge_sha="2" * 40,
                pr_head_sha=None,
                artifact_name=artifact_name,
            ),
        ),
        (
            "candidate SHA must equal PR head SHA",
            dict(
                event_name="pull_request",
                github_sha=checkout_sha,
                candidate_sha="1" * 40,
                merge_sha=checkout_sha,
                pr_head_sha="2" * 40,
                artifact_name=artifact_name,
            ),
        ),
        (
            "non-PR candidate SHA must equal GITHUB_SHA",
            dict(
                event_name="workflow_dispatch",
                github_sha=checkout_sha,
                candidate_sha="1" * 40,
                merge_sha=checkout_sha,
                pr_head_sha=None,
                artifact_name=artifact_name,
            ),
        ),
        (
            "artifact name must include job, GITHUB_SHA, run id, and run attempt",
            dict(
                event_name="push",
                github_sha=checkout_sha,
                candidate_sha=checkout_sha,
                merge_sha=checkout_sha,
                pr_head_sha=None,
                artifact_name=f"ci-evidence-linux-cmake-{checkout_sha}-456",
            ),
        ),
    )
    for expected_error, arguments in negative_cases:
        negative, _, _ = run_writer(**arguments)
        assert negative.returncode != 0, f"writer accepted invalid evidence identity: {expected_error}"
        assert expected_error in negative.stderr, negative.stderr


def verify_evidence_set_verifier(repo_root: Path) -> None:
    verifier = repo_root / "tools" / "verify_ci_evidence_set.py"
    assert verifier.is_file(), f"missing CI evidence-set verifier: {verifier}"
    verifier_text = verifier.read_text(encoding="utf-8")
    require(verifier_text, "stat::number_of_executed_units")
    require(verifier_text, "ASan libFuzzer execution count mismatch")
    require(verifier_text, "#1000")
    expected_jobs = tuple(JOB_CONTRACTS)
    consumer_jobs = {"linux-cmake", "windows-msvc", "windows-msvc-release"}
    github_sha = "a" * 40
    candidate_sha = "b" * 40

    def write_inventory(path: Path, total: int, *, main: bool) -> list[str]:
        names = [f"contract.synthetic.test_{index:03d}" for index in range(total)]
        tests = [
            {
                "name": name,
                "labels": ["contract", "threading"] if main and index < EXPECTED_THREADING_TOTAL else ["contract"],
            }
            for index, name in enumerate(names)
        ]
        if not main:
            names = ["gamenet.install_consumer"]
            tests = [{"name": names[0], "labels": []}]
        document = {
            "schema": "gamenet.ctest_inventory.v1",
            "generated_at_utc": "2026-07-11T00:00:00Z",
            "command": ["ctest", "--show-only=json-v1"],
            "total": len(names),
            "expected_total": len(names),
            "label_counts": {"threading": EXPECTED_THREADING_TOTAL} if main else {},
            "expected_label_counts": {},
            "tests": tests,
        }
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        return names

    def write_junit(path: Path, names: list[str]) -> None:
        cases = "".join(
            f'  <testcase name="{name}" classname="{name}" time="0.001" status="run" />\n'
            for name in names
        )
        path.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<testsuite name="synthetic" tests="{len(names)}" failures="0" errors="0" skipped="0">\n'
            f"{cases}</testsuite>\n",
            encoding="utf-8",
        )

    def create_fixture(root: Path, scenario: str = "positive") -> None:
        jobs = expected_jobs[:-1] if scenario == "missing-job" else expected_jobs
        for job in jobs:
            artifact_name = f"ci-evidence-{job}-{github_sha}-456-3"
            artifact = root / artifact_name
            artifact.mkdir(parents=True)
            (artifact / "toolchain.txt").write_text("synthetic toolchain\n", encoding="utf-8")
            main_total = 84 if scenario == "inventory-total" and job == "linux-release" else EXPECTED_CTEST_TOTAL
            main_names = write_inventory(artifact / "ctest-inventory.json", main_total, main=True)
            selected_names = main_names[:EXPECTED_THREADING_TOTAL] if job == "linux-tsan" else main_names
            if job == "linux-tsan":
                inventory = json.loads((artifact / "ctest-inventory.json").read_text(encoding="utf-8"))
                inventory["expected_label_counts"] = {"threading": EXPECTED_THREADING_TOTAL}
                (artifact / "ctest-inventory.json").write_text(
                    json.dumps(inventory, indent=2) + "\n", encoding="utf-8"
                )
            if scenario == "junit-count" and job == "linux-release":
                selected_names = selected_names[:-1]
            write_junit(artifact / "ctest-junit.xml", selected_names)
            (artifact / "ctest.log").write_text("100% tests passed\n", encoding="utf-8")

            if job in consumer_jobs and not (scenario == "missing-consumer" and job == "windows-msvc-release"):
                consumer_names = write_inventory(
                    artifact / "install-consumer-inventory.json", 1, main=False
                )
                write_junit(artifact / "install-consumer-junit.xml", consumer_names)
                (artifact / "install-consumer-ctest.log").write_text(
                    "100% tests passed\n", encoding="utf-8"
                )

            if job == "linux-asan-ubsan":
                fuzz = artifact / "fuzz"
                (fuzz / "corpus").mkdir(parents=True)
                fuzz_executions = 999 if scenario == "fuzz-short-run" else 1000
                fuzz_done_marker = 999 if scenario == "fuzz-missing-done-marker" else 1000
                (fuzz / "fuzzer.log").write_text(
                    f"#{fuzz_done_marker} DONE cov: 1 ft: 1 corp: 1/4b\n"
                    f"Done {fuzz_executions} runs in 1 second(s)\n"
                    f"stat::number_of_executed_units: {fuzz_executions}\n",
                    encoding="utf-8",
                )
                (fuzz / "packet_framer.dict").write_text('empty="\\x00"\n', encoding="utf-8")
                (fuzz / "corpus" / "seed").write_bytes(b"\x00\x00\x00\x00")

            evidence_files = []
            for path in sorted(artifact.rglob("*"), key=lambda item: item.as_posix()):
                if path.is_file():
                    evidence_files.append(
                        {
                            "path": path.relative_to(artifact).as_posix(),
                            "bytes": path.stat().st_size,
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        }
                    )
            job_candidate = "c" * 40 if scenario == "identity-mismatch" and job == "windows-msvc-release" else candidate_sha
            manifest = {
                "schema": "gamenet.ci_evidence.v1",
                "generated_at_utc": "2026-07-11T00:00:00Z",
                "date_utc": "2026-07-11",
                "checkout_sha": github_sha,
                "status": "success",
                "pre_upload_status": "success",
                "status_scope": "job_before_evidence_upload",
                "candidate_sha": job_candidate,
                "github_sha": github_sha,
                "pr_head_sha": job_candidate,
                "merge_or_current_sha": github_sha,
                "run_id": "456",
                "run_attempt": 3,
                "job": job,
                "workflow": "ci",
                "ref": "refs/pull/7/merge",
                "event_name": "pull_request",
                "repository": "YanqingXu/game-net-core",
                "runner": {"os": "Linux", "arch": "X64"},
                "artifact_name": artifact_name,
                "commands": ["ctest --output-junit ci-evidence/ctest-junit.xml"],
                "evidence_files": evidence_files,
            }
            (artifact / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            if scenario == "corrupt-hash" and job == "linux-cmake":
                (artifact / "ctest.log").write_text("corrupted after manifest\n", encoding="utf-8")

    def run_verifier(scenario: str) -> tuple[subprocess.CompletedProcess[str], dict[str, object] | None]:
        with tempfile.TemporaryDirectory(prefix="gamenet-ci-evidence-set-") as temporary:
            temporary_root = Path(temporary)
            input_root = temporary_root / "producers"
            input_root.mkdir()
            output = temporary_root / "aggregate" / "manifest.json"
            create_fixture(input_root, scenario)
            result = subprocess.run(
                [
                    sys.executable,
                    str(verifier),
                    "--input-root",
                    str(input_root),
                    "--output",
                    str(output),
                ],
                cwd=repo_root,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            aggregate = json.loads(output.read_text(encoding="utf-8")) if output.exists() else None
            return result, aggregate

    positive, aggregate = run_verifier("positive")
    assert positive.returncode == 0, positive.stderr
    assert aggregate is not None
    assert aggregate["schema"] == "gamenet.ci_evidence_set.v1"
    assert aggregate["status"] == "success"
    assert aggregate["github_sha"] == github_sha
    assert aggregate["candidate_sha"] == candidate_sha
    assert aggregate["run_id"] == "456"
    assert aggregate["run_attempt"] == 3
    assert aggregate["expected_jobs"] == list(expected_jobs)
    assert [producer["job"] for producer in aggregate["producers"]] == list(expected_jobs)
    assert all(len(producer["producer_manifest_sha256"]) == 64 for producer in aggregate["producers"])
    assert next(
        producer for producer in aggregate["producers"] if producer["job"] == "linux-tsan"
    )["executed_tests"] == EXPECTED_THREADING_TOTAL
    assert next(
        producer for producer in aggregate["producers"] if producer["job"] == "linux-asan-ubsan"
    )["fuzz_executed_units"] == 1000

    negative_cases = {
        "missing-job": "exactly six producer artifact directories",
        "identity-mismatch": "one run/attempt/SHA identity",
        "corrupt-hash": "mismatch for evidence file",
        "junit-count": "unexpected JUnit test count",
        "inventory-total": "unexpected inventory total",
        "missing-consumer": "missing install-consumer evidence",
        "fuzz-short-run": "ASan libFuzzer execution count mismatch",
        "fuzz-missing-done-marker": "ASan libFuzzer log lacks the exact #1000 DONE marker",
    }
    for scenario, expected_error in negative_cases.items():
        result, invalid_aggregate = run_verifier(scenario)
        assert result.returncode != 0, f"evidence-set verifier accepted negative fixture: {scenario}"
        assert invalid_aggregate is None
        assert expected_error in result.stderr, result.stderr


def verify_job_evidence_contract(job_name: str, job: str, contract: JobContract) -> None:
    assert job.count("- name: Record toolchain evidence") == 1
    assert job.count("- name: Verify CTest inventory") == 1
    assert job.count("- name: Write CI evidence manifest") == 1
    assert job.count("- name: Upload CI evidence") == 1
    require(job, f"timeout-minutes: {contract.timeout_minutes}")

    toolchain = step_block(job, "Record toolchain evidence")
    require(toolchain, "ci-evidence/toolchain.txt")
    require(toolchain, "cmake --version")
    require(toolchain, "ctest --version")
    require(toolchain, f"{contract.interpreter} --version")
    if contract.interpreter == "python3":
        require(toolchain, "set -euo pipefail")
        require(toolchain, "2>&1 | tee ci-evidence/toolchain.txt")
        require(toolchain, "c++ --version")
    else:
        require(toolchain, "Tee-Object -FilePath ci-evidence/toolchain.txt")
        require(toolchain, "vswhere.exe")

    configure = step_block(job, contract.configure_step)
    inventory = step_block(job, "Verify CTest inventory")
    build = step_block(job, contract.build_step)
    test = step_block(job, contract.test_step)
    manifest = step_block(job, "Write CI evidence manifest")
    upload = step_block(job, "Upload CI evidence")

    require(inventory, f"{contract.interpreter} tools/verify_ctest_inventory.py")
    require(inventory, f"--test-dir {contract.test_dir}")
    require(inventory, f"--expected-total {EXPECTED_CTEST_TOTAL}")
    require(inventory, "--output ci-evidence/ctest-inventory.json")
    if contract.config is not None:
        require(inventory, f"--config {contract.config}")
        require(test, f"-C {contract.config}")
    if contract.threading_inventory:
        require(inventory, f"--expect-label threading={EXPECTED_THREADING_TOTAL}")
        require(test, "-L threading")
    else:
        assert "--expect-label" not in inventory, f"unexpected label gate in {job_name}"

    require(test, "--output-on-failure")
    require(test, f"--timeout {contract.test_timeout}")
    require(test, '--output-junit "')
    require(test, "ci-evidence/ctest-junit.xml")
    require(test, '--output-log "')
    require(test, "ci-evidence/ctest.log")

    assert manifest.startswith("      - name: Write CI evidence manifest\n        if: always()")
    require(manifest, "GAMENET_CI_CANDIDATE_SHA")
    require(manifest, "GAMENET_CI_PR_HEAD_SHA")
    require(manifest, "GAMENET_CI_MERGE_OR_CURRENT_SHA")
    require(manifest, "GAMENET_CI_STATUS")
    require(manifest, f"{contract.interpreter} tools/write_ci_evidence.py")
    require(manifest, "--evidence-root ci-evidence")
    require(manifest, "--output ci-evidence/manifest.json")
    require(manifest, f"--artifact-name '{ARTIFACT_NAME}'")
    require(manifest, "--require-canonical-artifact-name")
    assert manifest.count("--artifact-name") == 1
    assert manifest.count("--command '") == contract.manifest_command_count

    assert upload.startswith("      - name: Upload CI evidence\n        if: always()")
    require(upload, "uses: actions/upload-artifact@v4")
    require(upload, f"name: {ARTIFACT_NAME}")
    require(upload, "path: ci-evidence/")
    require(upload, "if-no-files-found: error")
    require(upload, "retention-days: 90")
    for junit_path in contract.junit_paths:
        require(job, junit_path)
    assert job.count("--output-junit") == 2 * len(contract.junit_paths), (
        f"{job_name} must record each JUnit command in the workflow and manifest exactly once"
    )
    assert job.count("--output-log") == 2 * len(contract.junit_paths), (
        f"{job_name} must record each raw CTest log command in the workflow and manifest exactly once"
    )

    assert job.index(configure) < job.index(inventory) < job.index(build) < job.index(test)
    if contract.package_step is not None:
        package = step_block(job, contract.package_step)
        require(package, f"{contract.interpreter} tools/verify_ctest_inventory.py")
        require(package, "--expected-total 1")
        require(package, "--output ci-evidence/install-consumer-inventory.json")
        if contract.config is not None:
            require(package, f"--config {contract.config}")
        require(package, "--output-junit")
        require(package, "--output-log")
        require(package, "--timeout 60")
        assert package.index("tools/verify_ctest_inventory.py") < package.index("cmake --build")
        assert job.index(test) < job.index(package) < job.index(manifest)
    if contract.fuzz:
        fuzz = step_block(job, "Run PacketFramer libFuzzer smoke")
        require(fuzz, "set -euo pipefail")
        require(fuzz, "cmake -E copy_directory tests/fuzz/corpus/packet_framer build-fuzz/corpus/packet_framer")
        require(fuzz, "cmake -E copy_directory build-fuzz/corpus/packet_framer ci-evidence/fuzz/corpus")
        require(fuzz, "ci-evidence/fuzz/corpus")
        require(fuzz, "-dict=ci-evidence/fuzz/packet_framer.dict")
        require(fuzz, "-artifact_prefix=ci-evidence/fuzz/artifacts/")
        require(fuzz, "-seed=424242")
        require(fuzz, "-print_final_stats=1")
        require(fuzz, "-runs=1000")
        assert "-max_total_time" not in fuzz
        require(fuzz, "2>&1 | tee ci-evidence/fuzz/fuzzer.log")
        assert job.index(test) < job.index(fuzz) < job.index(manifest)
    assert job.index(manifest) < job.index(upload)
    assert job.rstrip().endswith("retention-days: 90")


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    workflow = (repo_root / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    ci_docs = (repo_root / "docs" / "development" / "ci.md").read_text(encoding="utf-8")

    require(workflow, "linux-cmake:")
    require(workflow, "Linux CMake build and tests")
    require(workflow, "linux-asan-ubsan:")
    require(workflow, "Linux ASan/UBSan build and tests")
    require(workflow, "linux-tsan:")
    require(workflow, "Linux TSan race-oriented build and tests")
    require(workflow, "linux-release:")
    require(workflow, "Linux Release build")
    require(workflow, "windows-msvc:")
    require(workflow, "Windows MSVC IOCP build and tests")
    require(workflow, "windows-msvc-release:")
    require(workflow, "Windows MSVC IOCP Release build and tests")
    require(workflow, "ci-evidence-set:")
    require(workflow, "Aggregate six-job CI evidence")
    require(workflow, "runs-on: windows-latest")
    require(workflow, "cancel-in-progress: true")
    require(workflow, "python3 tests/scope/test_intent_consistency.py")
    require(workflow, "python3 tests/scope/test_intent_metadata.py")
    require(workflow, "python tests/scope/test_intent_metadata.py")
    require(workflow, "python3 tests/scope/test_intent_semantics.py")
    require(workflow, "python tests/scope/test_intent_semantics.py")
    require(workflow, "python3 tests/cmake/test_install_package_contract.py")
    require(workflow, "python3 tests/cmake/test_packet_framer_fuzz_contract.py")
    require(workflow, "python3 tests/cmake/test_core_benchmark_contract.py")
    require(workflow, "python tests/cmake/test_core_benchmark_contract.py")
    require(workflow, "python3 tests/cmake/test_phase4_benchmark_contract.py")
    require(workflow, "python tests/cmake/test_phase4_benchmark_contract.py")
    require(workflow, "python tests/cmake/test_packet_framer_fuzz_contract.py")
    require(workflow, "python3 tests/cmake/test_logger_thread_contract.py")
    require(workflow, "python3 tests/cmake/test_event_loop_contracts.py")
    require(workflow, "python3 tests/cmake/test_event_loop_thread_pool_contracts.py")
    require(workflow, "python3 tests/cmake/test_migration_status_contract.py")
    require(workflow, "python3 tests/cmake/test_msvc_utf8_contract.py")
    require(workflow, "python3 tests/cmake/test_platform_backend_contract.py")
    require(workflow, "python3 tests/cmake/test_tcp_lifecycle_contracts.py")
    require(workflow, "python3 tests/cmake/test_tcp_connection_context_contract.py")
    require(workflow, "python3 tests/cmake/test_tcp_connection_thread_contract.py")
    require(workflow, "python3 tests/cmake/test_timer_queue_contracts.py")
    require(workflow, "python3 tests/cmake/test_threading_gate_contracts.py")
    require(workflow, "python3 tests/cmake/test_windows_iocp_milestone_contract.py")
    require(workflow, "python3 tests/cmake/test_windows_iocp_data_path_contract.py")
    require(workflow, "python3 tests/cmake/test_release_safe_tests.py")
    require(workflow, "python3 tests/ci/test_long_soak_workflow.py")
    require(workflow, "python3 tests/ci/test_core_benchmark_workflow.py")
    require(workflow, "python tests/ci/test_core_benchmark_workflow.py")
    require(workflow, "python3 tests/ci/test_phase4_benchmark_workflow.py")
    require(workflow, "python tests/ci/test_phase4_benchmark_workflow.py")
    require(workflow, "Configure PacketFramer libFuzzer smoke")
    require(workflow, "-DGAMENET_BUILD_FUZZING=ON")
    require(workflow, "--target gamenet_fuzz_packet_framer")
    require(workflow, "tests/fuzz/corpus/packet_framer")
    require(workflow, "-runs=1000")
    require(workflow, "-DCMAKE_BUILD_TYPE=Release")
    require(workflow, "-DGAMENET_BUILD_TESTING=ON")
    require(workflow, "-DGAMENET_ENABLE_TSAN=ON")
    require(workflow, "-DGAMENET_ENABLE_TLS=OFF")
    require(workflow, "-DGAMENET_ENABLE_EXPERIMENTAL=OFF")
    require(workflow, "cmake --install build --prefix \"$PWD/build/_install\"")
    require(workflow, "cmake -S tests/cmake/install_consumer")
    require(workflow, "-DCMAKE_PREFIX_PATH=\"$PWD/build/_install\"")
    require(workflow, "ctest --test-dir build-release --output-on-failure")
    require(workflow, "ctest --test-dir build-tsan --output-on-failure -L threading --timeout 60")
    require(workflow, "-G \"Visual Studio 18 2026\"")
    require(workflow, "-A x64")
    require(workflow, "cmake --build build-windows --config Debug --parallel")
    require(workflow, "ctest --test-dir build-windows -C Debug --output-on-failure --timeout 10")
    require(workflow, "python tests/ci/test_long_soak_workflow.py")
    require(workflow, "cmake --install build-windows --config Debug --prefix \"$pwd/build-windows/_install\"")
    require(workflow, "cmake -S tests/cmake/install_consumer -B build-windows-install-consumer")
    require(workflow, "-DCMAKE_PREFIX_PATH=\"$pwd/build-windows/_install\"")
    require(workflow, "cmake --build build-windows-install-consumer --config Debug --parallel")
    require(workflow, "ctest --test-dir build-install-consumer --output-on-failure")
    require(
        workflow,
        "ctest --test-dir build-windows-install-consumer -C Debug --output-on-failure",
    )

    for job_name, interpreter in PROVENANCE_JOBS:
        job = job_block(workflow, job_name)
        source_checkout = step_block(job, "Checkout migration provenance source")
        require(source_checkout, "uses: actions/checkout@v4")
        require(source_checkout, f"repository: {SOURCE_REPOSITORY}")
        require(source_checkout, f"ref: {SOURCE_COMMIT}")
        require(source_checkout, "path: mini_trantor")
        require(source_checkout, "persist-credentials: false")

        guards = step_block(job, "Check repository guards")
        verifier = f"{interpreter} tools/verify_migration_provenance.py"
        semantic_guard = f"{interpreter} tests/scope/test_intent_semantics.py"
        require(guards, verifier)
        require(guards, semantic_guard)
        assert guards.index(verifier) < guards.index(semantic_guard), (
            f"{job_name} must verify immutable migration provenance before intent semantics"
        )
        assert job.index(source_checkout) < job.index(guards), (
            f"{job_name} must checkout migration provenance before repository guards"
        )

    assert workflow.count("- name: Checkout migration provenance source") == len(PROVENANCE_JOBS)
    assert workflow.count(f"repository: {SOURCE_REPOSITORY}") == len(PROVENANCE_JOBS)
    assert workflow.count(f"ref: {SOURCE_COMMIT}") == len(PROVENANCE_JOBS)

    for job_name, contract in JOB_CONTRACTS.items():
        verify_job_evidence_contract(job_name, job_block(workflow, job_name), contract)

    aggregate_job = job_block(workflow, "ci-evidence-set")
    require(aggregate_job, "if: always()")
    require(aggregate_job, "timeout-minutes: 10")
    for job_name in JOB_CONTRACTS:
        require(aggregate_job, f"- {job_name}")
    download = step_block(aggregate_job, "Download six producer evidence artifacts")
    require(download, "uses: actions/download-artifact@v4")
    require(download, "pattern: ci-evidence-*-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}")
    require(download, "path: ci-evidence-producers")
    require(download, "merge-multiple: false")
    aggregate = step_block(aggregate_job, "Verify and aggregate six-job evidence")
    require(aggregate, "python3 tools/verify_ci_evidence_set.py")
    require(aggregate, "--input-root ci-evidence-producers")
    require(aggregate, "--output ci-evidence-aggregate/manifest.json")
    aggregate_upload = step_block(aggregate_job, "Upload aggregate CI evidence")
    require(aggregate_upload, "if: always()")
    require(aggregate_upload, "uses: actions/upload-artifact@v4")
    require(aggregate_upload, f"name: {AGGREGATE_ARTIFACT_NAME}")
    require(aggregate_upload, "path: ci-evidence-aggregate/")
    require(aggregate_upload, "if-no-files-found: error")
    require(aggregate_upload, "retention-days: 90")
    assert aggregate_job.index(download) < aggregate_job.index(aggregate) < aggregate_job.index(aggregate_upload)

    assert workflow.count("- name: Record toolchain evidence") == len(JOB_CONTRACTS)
    assert workflow.count("- name: Verify CTest inventory") == len(JOB_CONTRACTS)
    assert workflow.count("- name: Write CI evidence manifest") == len(JOB_CONTRACTS)
    assert workflow.count("- name: Upload CI evidence") == len(JOB_CONTRACTS)
    assert workflow.count("uses: actions/upload-artifact@v4") == len(JOB_CONTRACTS) + 1
    assert workflow.count("uses: actions/download-artifact@v4") == 1
    assert workflow.count("--artifact-name") == len(JOB_CONTRACTS)
    assert workflow.count("--require-canonical-artifact-name") == len(JOB_CONTRACTS)
    assert workflow.count(f"name: {ARTIFACT_NAME}") == len(JOB_CONTRACTS)
    assert workflow.count("retention-days: 90") == len(JOB_CONTRACTS) + 1
    assert workflow.count(f"--expected-total {EXPECTED_CTEST_TOTAL}") == 2 * len(JOB_CONTRACTS)
    assert workflow.count("--expected-total 1") == 6
    assert workflow.count(f"--expect-label threading={EXPECTED_THREADING_TOTAL}") == 2
    assert workflow.count("path: ci-evidence/") == len(JOB_CONTRACTS)
    assert workflow.count("if-no-files-found: error") == len(JOB_CONTRACTS) + 1
    verify_manifest_writer(repo_root)
    verify_evidence_set_verifier(repo_root)

    require(ci_docs, "tools/verify_migration_provenance.py")
    require(ci_docs, SOURCE_REPOSITORY)
    require(ci_docs, SOURCE_COMMIT)
    require(ci_docs, "Git object")
    require(ci_docs, "tools/verify_ctest_inventory.py")
    require(ci_docs, "tools/write_ci_evidence.py")
    require(ci_docs, "tools/verify_ci_evidence_set.py")
    require(ci_docs, "gamenet.ci_evidence.v1")
    require(ci_docs, "gamenet.ci_evidence_set.v1")
    require(ci_docs, "exactly 85")
    require(ci_docs, "threading=61")
    require(ci_docs, "exactly 1")
    require(ci_docs, "--output-junit")
    require(ci_docs, "--output-log")
    require(ci_docs, "90 days")
    require(ci_docs, "artifact_name")
    require(ci_docs, "job_before_evidence_upload")
    require(ci_docs, "CTest 3.21 or newer")
    require(ci_docs, "GitHub Actions job metadata")
    require(ci_docs, "checkout_sha")
    require(ci_docs, "pre_upload_status")
    require(ci_docs, "runner")

    linux_debug = job_block(workflow, "linux-cmake")
    windows_debug = job_block(workflow, "windows-msvc")
    windows_release = job_block(workflow, "windows-msvc-release")

    require(linux_debug, "cmake --build build-install-consumer --parallel")
    require(linux_debug, "ctest --test-dir build-install-consumer --output-on-failure")
    require(windows_debug, "cmake --build build-windows-install-consumer --config Debug --parallel")
    require(
        windows_debug,
        "ctest --test-dir build-windows-install-consumer -C Debug --output-on-failure",
    )
    require(windows_release, "cmake -S . -B build-windows-release")
    require(windows_release, "cmake --build build-windows-release --config Release --parallel")
    require(
        windows_release,
        "ctest --test-dir build-windows-release -C Release --output-on-failure --timeout 10",
    )
    require(
        windows_release,
        'cmake --install build-windows-release --config Release --prefix "$pwd/build-windows-release/_install"',
    )
    require(
        windows_release,
        "cmake -S tests/cmake/install_consumer -B build-windows-release-install-consumer",
    )
    require(
        windows_release,
        'DCMAKE_PREFIX_PATH="$pwd/build-windows-release/_install"',
    )
    require(
        windows_release,
        "cmake --build build-windows-release-install-consumer --config Release --parallel",
    )
    require(
        windows_release,
        "ctest --test-dir build-windows-release-install-consumer -C Release --output-on-failure",
    )


if __name__ == "__main__":
    main()
