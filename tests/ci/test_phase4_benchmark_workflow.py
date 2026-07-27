from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing Phase 4 benchmark workflow fragment in {source}: {needle}"


def job_block(workflow: str, job_name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(job_name)}:\n(?P<body>.*?)(?=^  [a-z0-9_-]+:\n|\Z)",
        workflow,
    )
    assert match is not None, f"missing benchmark workflow job: {job_name}"
    return match.group(0)


def step_block(job: str, step_name: str) -> str:
    match = re.search(
        rf"(?ms)^      - name: {re.escape(step_name)}\n(?P<body>.*?)(?=^      - name: |\Z)",
        job,
    )
    assert match is not None, f"missing benchmark workflow step: {step_name}"
    return match.group(0)


def run_validator(
    validator: Path,
    input_dir: Path,
    *extra: str,
    platform: str = "windows",
    backend: str = "iocp",
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(validator),
            "--input-dir",
            str(input_dir),
            "--platform",
            platform,
            "--backend",
            backend,
            "--build-type",
            "Release",
            *extra,
        ],
        check=False,
        capture_output=True,
        text=True,
    )


def expect_validation_failure(
    validator: Path,
    source_results: Path,
    scenario: str,
    mutation,
    expected_message: str,
) -> None:
    with tempfile.TemporaryDirectory(prefix="gamenet-phase4-benchmark-negative-") as temp:
        fixture = Path(temp) / "results"
        shutil.copytree(source_results, fixture)
        path = fixture / f"{scenario}.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        mutation(document)
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        result = run_validator(validator, fixture)
        assert result.returncode != 0, f"negative benchmark fixture unexpectedly passed: {scenario}"
        assert expected_message in result.stderr, (
            f"negative benchmark fixture failed for the wrong reason: {result.stderr}"
        )


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_job_manifest(root: Path, job: str, artifact_name: str, sha: str) -> None:
    files = []
    for path in sorted(root.rglob("*")):
        if path.name == "job-manifest.json" or not path.is_file():
            continue
        files.append(
            {
                "path": path.relative_to(root).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    manifest = {
        "schema": "gamenet.ci_evidence.v1",
        "checkout_sha": sha,
        "status": "success",
        "pre_upload_status": "success",
        "candidate_sha": sha,
        "github_sha": sha,
        "pr_head_sha": None,
        "merge_or_current_sha": sha,
        "run_id": "12345",
        "run_attempt": 2,
        "job": job,
        "workflow": "core-benchmark",
        "ref": "refs/heads/main",
        "event_name": "workflow_dispatch",
        "repository": "YanqingXu/game-net-core",
        "runner": {
            "os": "Windows" if job.startswith("windows") else "Linux",
            "arch": "X64",
        },
        "artifact_name": artifact_name,
        "commands": ["fixture benchmark command"],
        "evidence_files": files,
    }
    (root / "job-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )


def write_performance_fixture(
    root: Path,
    candidate_sha: str,
    platform: str,
    backend: str,
    budget: dict,
) -> None:
    for identity, commit_sha in (("baseline", budget["baseline_sha"]), ("candidate", candidate_sha)):
        matrix_root = root / "performance-samples" / identity
        matrix_root.mkdir(parents=True)
        scenarios = []
        for scenario_budget in budget["scenarios"]:
            key = scenario_budget["key"]
            group, short_key = key.split(".", 1)
            samples = []
            for repetition in range(1, budget["repetitions"] + 1):
                relative = Path(group) / f"{short_key}-{repetition}.json"
                sample = matrix_root / relative
                sample.parent.mkdir(exist_ok=True)
                sample.write_text(json.dumps({"fixture": key, "repetition": repetition}) + "\n", encoding="utf-8")
                samples.append({"path": relative.as_posix(), "sha256": sha256_file(sample)})
            scenarios.append({"key": key, "samples": samples})
        matrix = {
            "schema": "gamenet.performance_matrix.v1",
            "commit_sha": commit_sha,
            "platform": platform,
            "backend": backend,
            "build_type": "Release",
            "repetitions": budget["repetitions"],
            "scenarios": scenarios,
        }
        (matrix_root / "matrix-manifest.json").write_text(
            json.dumps(matrix, indent=2) + "\n", encoding="utf-8"
        )

    comparisons = []
    for scenario_budget in budget["scenarios"]:
        metrics = []
        for metric in scenario_budget["metrics"]:
            metrics.append({
                **metric,
                "baseline_samples": [100.0] * budget["repetitions"],
                "candidate_samples": [100.0] * budget["repetitions"],
                "baseline_median": 100.0,
                "candidate_median": 100.0,
                "threshold": 100.0,
                "result": "pass",
            })
        comparisons.append({"key": scenario_budget["key"], "metrics": metrics, "result": "pass"})
    performance = {
        "schema": "gamenet.performance_regression.v1",
        "result": "pass",
        "platform": platform,
        "backend": backend,
        "build_type": "Release",
        "baseline_sha": budget["baseline_sha"],
        "candidate_sha": candidate_sha,
        "repetitions": budget["repetitions"],
        "budget_sha256": "f" * 64,
        "comparisons": comparisons,
    }
    (root / "performance-regression.json").write_text(
        json.dumps(performance, indent=2) + "\n", encoding="utf-8"
    )


def verify_pair_tool(
    repo_root: Path,
    validator: Path,
    linux_results: Path,
    windows_results: Path,
) -> None:
    pair_verifier = repo_root / "tools" / "verify_phase4_benchmark_evidence_set.py"
    assert pair_verifier.is_file(), f"missing Phase 4 benchmark pair verifier: {pair_verifier}"
    sha = "a" * 40
    budget = json.loads(
        (repo_root / "benchmarks" / "performance_regression_budgets.json").read_text(encoding="utf-8")
    )
    with tempfile.TemporaryDirectory(prefix="gamenet-phase4-benchmark-pair-") as temp:
        evidence_root = Path(temp) / "evidence"
        evidence_root.mkdir()
        fixtures = (
            (
                "linux-release-benchmark",
                "linux",
                "epoll",
                linux_results,
            ),
            (
                "windows-release-benchmark",
                "windows",
                "iocp",
                windows_results,
            ),
        )
        for job, platform, backend, source in fixtures:
            artifact_name = f"phase4-benchmark-{job}-{sha}-12345-2"
            root = evidence_root / artifact_name
            root.mkdir()
            for scenario in ("framing", "logic-queue", "broadcast-fanout"):
                shutil.copy2(source / f"{scenario}.json", root / f"{scenario}.json")
            (root / "toolchain.txt").write_text("fixture toolchain\n", encoding="utf-8")
            result = run_validator(
                validator,
                root,
                "--commit-sha",
                sha,
                "--run-id",
                "12345",
                "--job",
                job,
                "--artifact-name",
                artifact_name,
                "--manifest-output",
                str(root / "evidence-manifest.json"),
                platform=platform,
                backend=backend,
            )
            assert result.returncode == 0, result.stderr
            write_performance_fixture(root, sha, platform, backend, budget)
            write_job_manifest(root, job, artifact_name, sha)

        output = evidence_root / "pair-manifest.json"
        command = [
            sys.executable,
            str(pair_verifier),
            "--input-root",
            str(evidence_root),
            "--output",
            str(output),
        ]
        positive = subprocess.run(command, capture_output=True, text=True, check=False)
        assert positive.returncode == 0, positive.stderr
        pair = json.loads(output.read_text(encoding="utf-8"))
        assert pair["schema"] == "gamenet.phase4_benchmark_pair_evidence.v1"
        assert pair["result"] == "success"
        assert len(pair["platforms"]) == 2

        windows_manifest = next(
            path
            for path in evidence_root.rglob("job-manifest.json")
            if "windows-release-benchmark" in path.parent.name
        )
        mismatch = json.loads(windows_manifest.read_text(encoding="utf-8"))
        mismatch["candidate_sha"] = "b" * 40
        windows_manifest.write_text(json.dumps(mismatch, indent=2) + "\n", encoding="utf-8")
        negative = subprocess.run(command, capture_output=True, text=True, check=False)
        assert negative.returncode != 0
        assert "identities do not match" in negative.stderr


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    workflow = repo_root / ".github" / "workflows" / "core-benchmark.yml"
    ci_workflow = repo_root / ".github" / "workflows" / "ci.yml"
    long_soak = repo_root / ".github" / "workflows" / "long-soak.yml"
    docs = repo_root / "docs" / "development" / "phase4_benchmark.md"
    validator = repo_root / "tools" / "validate_phase4_benchmark.py"
    windows_results = (
        repo_root
        / "docs"
        / "development"
        / "benchmark_results"
        / "2026-07-11-windows-msvc-release-phase4"
    )
    linux_results = (
        repo_root
        / "docs"
        / "development"
        / "benchmark_results"
        / "2026-07-11-linux-clang-release-phase4"
    )

    text = workflow.read_text(encoding="utf-8")
    require(text, "workflow_dispatch:", workflow)
    assert "\n  push:" not in text, "benchmark workflow must not run on push"
    assert "\n  pull_request:" not in text, "benchmark workflow must not run on pull request"
    permissions = re.search(r"(?m)^permissions:\n(?P<body>(?:  [^\n]+\n)+)", text)
    assert permissions is not None, "benchmark workflow needs explicit permissions"
    assert permissions.group("body") == "  contents: read\n", (
        "benchmark workflow must retain contents: read as its only repository permission"
    )

    linux = job_block(text, "linux-release-benchmark")
    windows = job_block(text, "windows-release-benchmark")
    pair_job = job_block(text, "phase4-benchmark-evidence")
    require(linux, "Linux Release epoll benchmark", workflow)
    require(windows, "Windows Release IOCP benchmark", workflow)
    require(linux, "gamenet_phase4_benchmark", workflow)
    require(windows, "gamenet_phase4_benchmark", workflow)
    require(pair_job, "Verify paired Phase 4 benchmark evidence", workflow)

    linux_candidate = step_block(linux, "Run candidate performance matrix")
    windows_candidate = step_block(windows, "Run candidate performance matrix")
    linux_baseline = step_block(linux, "Run baseline performance matrix")
    windows_baseline = step_block(windows, "Run baseline performance matrix")
    for step, interpreter, platform, backend in (
        (linux_candidate, "python3", "linux", "epoll"),
        (windows_candidate, "python", "windows", "iocp"),
        (linux_baseline, "python3", "linux", "epoll"),
        (windows_baseline, "python", "windows", "iocp"),
    ):
        require(step, f"{interpreter} tools/run_performance_matrix.py", workflow)
        require(step, f"--platform {platform} --backend {backend}", workflow)
        require(step, "--repetitions 3", workflow)
    require(linux_candidate, "--canonical-phase4-dir phase4-benchmark-results", workflow)
    require(windows_candidate, "--canonical-phase4-dir phase4-benchmark-results", workflow)
    require(linux_baseline, "performance-samples/baseline", workflow)
    require(windows_baseline, "performance-samples/baseline", workflow)

    runner = repo_root / "tools" / "run_performance_matrix.py"
    runner_text = runner.read_text(encoding="utf-8")
    for fragment in (
        '"framing", "framing"',
        '"logic-queue", "logic-queue"',
        '"logic-queue-heavy", "logic-queue"',
        '"broadcast-fanout", "broadcast-fanout"',
        '"broadcast-fanout-4096", "broadcast-fanout"',
        '"--fanout", "4096"',
    ):
        require(runner_text, fragment, runner)

    linux_validation = step_block(linux, "Validate Phase 4 JSON artifacts")
    require(linux_validation, "python3 tools/validate_phase4_benchmark.py", workflow)
    require(linux_validation, "--platform linux", workflow)
    require(linux_validation, "--backend epoll", workflow)
    require(linux_validation, '--commit-sha "${GITHUB_SHA}"', workflow)
    require(linux_validation, '--run-id "${GITHUB_RUN_ID}"', workflow)
    require(linux_validation, '--job "${GITHUB_JOB}"', workflow)
    require(
        linux_validation,
        '--artifact-name "phase4-benchmark-${GITHUB_JOB}-${GITHUB_SHA}-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}"',
        workflow,
    )
    require(linux_validation, "--manifest-output phase4-benchmark-results/evidence-manifest.json", workflow)

    windows_validation = step_block(windows, "Validate Phase 4 JSON artifacts")
    require(windows_validation, "python tools/validate_phase4_benchmark.py", workflow)
    require(windows_validation, "--platform windows", workflow)
    require(windows_validation, "--backend iocp", workflow)
    require(windows_validation, '--commit-sha "$env:GITHUB_SHA"', workflow)
    require(windows_validation, '--run-id "$env:GITHUB_RUN_ID"', workflow)
    require(windows_validation, '--job "$env:GITHUB_JOB"', workflow)
    require(
        windows_validation,
        '--artifact-name "phase4-benchmark-$($env:GITHUB_JOB)-$($env:GITHUB_SHA)-$($env:GITHUB_RUN_ID)-$($env:GITHUB_RUN_ATTEMPT)"',
        workflow,
    )
    require(windows_validation, "--manifest-output phase4-benchmark-results/evidence-manifest.json", workflow)

    linux_writer = step_block(linux, "Write Linux Phase 4 benchmark job evidence")
    windows_writer = step_block(windows, "Write Windows Phase 4 benchmark job evidence")
    for writer in (linux_writer, windows_writer):
        require(writer, "if: always()", workflow)
        require(writer, "tools/write_ci_evidence.py", workflow)
        require(writer, "--output phase4-benchmark-results/job-manifest.json", workflow)
        require(writer, "--require-canonical-artifact-name", workflow)
        require(
            writer,
            "phase4-benchmark-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}",
            workflow,
        )

    linux_upload = step_block(linux, "Upload Linux Phase 4 raw JSON")
    windows_upload = step_block(windows, "Upload Windows Phase 4 raw JSON")
    require(
        linux_upload,
        "name: phase4-benchmark-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}",
        workflow,
    )
    require(
        windows_upload,
        "name: phase4-benchmark-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}",
        workflow,
    )
    for upload in (linux_upload, windows_upload):
        require(upload, "if: always()", workflow)
        require(upload, "uses: actions/upload-artifact@v4", workflow)
        require(upload, "path: phase4-benchmark-results/", workflow)
        require(upload, "if-no-files-found: error", workflow)
        require(upload, "retention-days: 90", workflow)

    require(pair_job, "needs:", workflow)
    require(pair_job, "- linux-release-benchmark", workflow)
    require(pair_job, "- windows-release-benchmark", workflow)
    require(pair_job, "if: always()", workflow)
    producer_gate = step_block(pair_job, "Require successful benchmark producers")
    assert pair_job.index("- name: Require successful benchmark producers") < pair_job.index(
        "- name: Checkout"
    ), "producer final-result gate must be the paired job's first step"
    require(
        producer_gate,
        "LINUX_PRODUCER_RESULT: ${{ needs['linux-release-benchmark'].result }}",
        workflow,
    )
    require(
        producer_gate,
        "WINDOWS_PRODUCER_RESULT: ${{ needs['windows-release-benchmark'].result }}",
        workflow,
    )
    require(producer_gate, '[ "$LINUX_PRODUCER_RESULT" != "success" ]', workflow)
    require(producer_gate, '[ "$WINDOWS_PRODUCER_RESULT" != "success" ]', workflow)
    require(producer_gate, "exit 1", workflow)
    assert "continue-on-error: true" not in producer_gate, (
        "paired evidence must not ignore a failed producer final-result gate"
    )
    assert "|| true" not in producer_gate and "exit 0" not in producer_gate, (
        "producer final-result gate must fail closed"
    )
    download = step_block(pair_job, "Download Phase 4 benchmark job evidence")
    require(download, "uses: actions/download-artifact@v4", workflow)
    require(
        download,
        "pattern: phase4-benchmark-*-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}",
        workflow,
    )
    pair_validation = step_block(pair_job, "Verify paired Phase 4 benchmark evidence")
    require(pair_validation, "tools/verify_phase4_benchmark_evidence_set.py", workflow)
    require(pair_validation, "--input-root phase4-benchmark-evidence-set", workflow)
    require(pair_validation, "--output phase4-benchmark-evidence-set/pair-manifest.json", workflow)
    for downstream in (download, pair_validation):
        assert "if: always()" not in downstream, (
            "download/pair verification must not bypass a failed producer final-result gate"
        )

    for job in (linux, windows):
        regression = step_block(job, "Enforce same-runner performance budgets")
        require(regression, "tools/compare_performance_regression.py", workflow)
        require(regression, "performance_regression_budgets.json", workflow)
        require(regression, "performance-regression.json", workflow)

    assert validator.is_file(), f"missing shared Phase 4 benchmark validator: {validator}"
    validator_text = validator.read_text(encoding="utf-8")
    for fragment in (
        "gamenet.phase4_benchmark.v1",
        "gamenet.phase4_benchmark_evidence.v1",
        "logic accepted count must equal configured messages",
        "broadcast accepted count must equal messages times fanout",
        "broadcast scheduled-task count does not match owner grouping and batch size",
        "no performance thresholds",
    ):
        require(validator_text, fragment, validator)

    windows_local = run_validator(validator, windows_results)
    assert windows_local.returncode == 0, windows_local.stderr
    linux_local = run_validator(
        validator,
        linux_results,
        platform="linux",
        backend="epoll",
    )
    assert linux_local.returncode == 0, linux_local.stderr
    verify_pair_tool(repo_root, validator, linux_results, windows_results)
    expect_validation_failure(
        validator,
        windows_results,
        "framing",
        lambda document: document["measurements"].__setitem__("throughput_mib_per_second", None),
        "framing throughput MiB/s must be a finite positive number",
    )
    expect_validation_failure(
        validator,
        windows_results,
        "logic-queue",
        lambda document: document["measurements"].__setitem__(
            "logic_accepted", document["parameters"]["messages"] - 1
        ),
        "logic accepted count must equal configured messages",
    )
    expect_validation_failure(
        validator,
        windows_results,
        "broadcast-fanout",
        lambda document: document["measurements"].__setitem__(
            "broadcast_scheduled_tasks", document["measurements"]["broadcast_scheduled_tasks"] + 1
        ),
        "broadcast scheduled-task count does not match owner grouping and batch size",
    )

    with tempfile.TemporaryDirectory(prefix="gamenet-phase4-benchmark-manifest-") as temp:
        manifest = Path(temp) / "evidence-manifest.json"
        manifest_result = run_validator(
            validator,
            windows_results,
            "--commit-sha",
            "a" * 40,
            "--run-id",
            "12345",
            "--job",
            "windows-release-benchmark",
            "--artifact-name",
            "phase4-benchmark-windows-release-" + "a" * 40 + "-12345",
            "--manifest-output",
            str(manifest),
        )
        assert manifest_result.returncode == 0, manifest_result.stderr
        evidence = json.loads(manifest.read_text(encoding="utf-8"))
        assert evidence["schema"] == "gamenet.phase4_benchmark_evidence.v1"
        assert evidence["commit_sha"] == "a" * 40
        assert evidence["run_id"] == "12345"
        assert len(evidence["artifacts"]) == 3
        assert all(len(item["sha256"]) == 64 for item in evidence["artifacts"])

    guard = "tests/ci/test_phase4_benchmark_workflow.py"
    require(ci_workflow.read_text(encoding="utf-8"), guard, ci_workflow)
    require(long_soak.read_text(encoding="utf-8"), guard, long_soak)

    docs_text = docs.read_text(encoding="utf-8")
    require(docs_text, "same runner", docs)
    require(docs_text, "baseline and candidate", docs)
    require(docs_text, "raw JSON artifacts", docs)
    require(docs_text, "Linux epoll", docs)
    require(docs_text, "Windows IOCP", docs)
    require(docs_text, "producer final result", docs)


if __name__ == "__main__":
    main()
