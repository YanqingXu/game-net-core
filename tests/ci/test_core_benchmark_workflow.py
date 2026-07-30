from __future__ import annotations

import re
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing core benchmark workflow fragment in {source}: {needle}"


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


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    workflow = repo_root / ".github" / "workflows" / "core-benchmark.yml"
    ci_workflow = repo_root / ".github" / "workflows" / "ci.yml"
    long_soak = repo_root / ".github" / "workflows" / "long-soak.yml"
    docs = repo_root / "docs" / "development" / "core_benchmark.md"

    assert workflow.exists(), f"missing manual core benchmark workflow: {workflow}"
    text = workflow.read_text(encoding="utf-8")
    require(text, "name: core-benchmark", workflow)
    require(text, "workflow_dispatch:", workflow)
    assert "\n  push:" not in text, "benchmark workflow must not run on push"
    assert "\n  pull_request:" not in text, "benchmark workflow must not run on pull request"
    require(text, "Linux Release epoll benchmark", workflow)
    require(text, "Windows Release IOCP benchmark", workflow)
    require(text, "runs-on: ubuntu-24.04", workflow)
    require(text, "runs-on: windows-latest", workflow)
    require(text, "-DCMAKE_BUILD_TYPE=Release", workflow)
    require(text, 'GAMENET_BUILD_BENCHMARKS=ON', workflow)
    require(text, "--config Release", workflow)
    runner = repo_root / "tools" / "run_performance_matrix.py"
    runner_text = runner.read_text(encoding="utf-8")
    require(runner_text, '"--scenario", "echo"', runner)
    require(runner_text, '"--scenario", "connections"', runner)
    require(runner_text, '"--scenario", "slow-client"', runner)
    require(runner_text, "gamenet.core_benchmark.v1", runner)
    require(runner_text, "gamenet.core_benchmark.v2", runner)
    require(runner_text, '"echo-4-workers"', runner)
    require(runner_text, '"connections-1024"', runner)
    require(runner_text, '"slow-client-16"', runner)
    require(runner_text, "CAPACITY_SCENARIOS", runner)
    require(runner_text, '"connections-10000"', runner)
    require(runner_text, '"connection-churn-1000"', runner)
    require(runner_text, '"core-capacity"', runner)
    validator = repo_root / "tools" / "validate_core_benchmark.py"
    validator_text = validator.read_text(encoding="utf-8")
    require(validator_text, 'SCHEMA = "gamenet.core_benchmark.v2"', validator)
    require(
        validator_text,
        'fields["requested_bytes"] == fields["accepted_bytes"] + fields["rejected_bytes"]',
        validator,
    )
    require(
        validator_text,
        'fields["pending_output_peak_bytes"] <= fields["output_hard_limit_bytes"]',
        validator,
    )
    require(text, "actions/upload-artifact@v4", workflow)
    assert text.count("Verify Core hard-limit benchmark semantics") == 2
    assert text.count("tools/validate_core_benchmark.py") >= 6
    assert text.count("--slow-bytes 33554432") >= 2
    assert text.count("--expected-connections 2") == 2
    assert text.count("--expected-slow-bytes 33554432") == 2
    assert text.count("--require-overload") >= 4
    assert text.count("Checkout Core capacity baseline") == 2
    assert text.count("Run candidate Core capacity matrix") == 2
    assert text.count("Run baseline Core capacity matrix") == 2
    assert text.count("Enforce same-runner Core capacity budgets") == 2
    assert text.count("--matrix-profile core-capacity") >= 6
    assert text.count("core_capacity_regression_budgets.json") >= 4
    assert text.count("core-capacity-regression.json") == 2
    assert text.count("Evaluate Linux accept topology") == 1
    assert text.count("core-accept-topology-decision.json") >= 1
    assert "ConvertFrom-Json" not in text
    canonical_artifact_name = (
        "core-benchmark-${{ github.job }}-${{ github.sha }}-"
        "${{ github.run_id }}-${{ github.run_attempt }}"
    )
    linux_upload = step_block(
        job_block(text, "linux-release-benchmark"), "Upload Linux raw JSON"
    )
    windows_upload = step_block(
        job_block(text, "windows-release-benchmark"), "Upload Windows raw JSON"
    )
    for upload in (linux_upload, windows_upload):
        require(upload, f"name: {canonical_artifact_name}", workflow)
        require(upload, "uses: actions/upload-artifact@v4", workflow)
        require(upload, "path: benchmark-results/*.json", workflow)
        require(upload, "if-no-files-found: error", workflow)
        require(upload, "retention-days: 90", workflow)
    assert "name: core-benchmark-linux-release-${{ github.sha }}" not in text, (
        "SHA-only Linux Core artifacts collide when a workflow run is rerun"
    )
    assert "name: core-benchmark-windows-release-${{ github.sha }}" not in text, (
        "SHA-only Windows Core artifacts collide when a workflow run is rerun"
    )
    assert "throughput_mib_per_second" not in text, (
        "workflow must keep metric budgets in the reviewed JSON contract"
    )

    guard = "tests/ci/test_core_benchmark_workflow.py"
    require(ci_workflow.read_text(encoding="utf-8"), guard, ci_workflow)
    require(long_soak.read_text(encoding="utf-8"), guard, long_soak)

    docs_text = docs.read_text(encoding="utf-8")
    require(docs_text, "core-benchmark", docs)
    require(docs_text, "same runner", docs)
    require(docs_text, "baseline and candidate", docs)
    require(docs_text, "raw JSON artifacts", docs)
    require(docs_text, "run attempt", docs)


if __name__ == "__main__":
    main()
