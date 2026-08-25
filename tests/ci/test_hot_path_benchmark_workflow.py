# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import re
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing HP0 workflow fragment in {source}: {needle}"


def job_block(text: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        text,
    )
    assert match is not None, f"missing workflow job: {name}"
    return match.group(0)


def main() -> None:
    repo = Path(__file__).resolve().parents[2]
    workflow = repo / ".github" / "workflows" / "hot-path-benchmark.yml"
    ci = repo / ".github" / "workflows" / "ci.yml"
    long_soak = repo / ".github" / "workflows" / "long-soak.yml"
    text = workflow.read_text(encoding="utf-8")

    require(text, "workflow_dispatch:", workflow)
    assert "\n  push:" not in text, "fixed-lab workflow must not run on push"
    assert "\n  pull_request:" not in text, "fixed-lab workflow must not run on pull request"
    require(text, "permissions:\n  contents: read", workflow)
    require(text, "frequency_policy:", workflow)
    require(text, "linux_observer:", workflow)
    require(text, "windows_observer:", workflow)

    linux = job_block(text, "linux-fixed-hot-path")
    windows = job_block(text, "windows-fixed-hot-path")
    for job, platform, backend, observer in (
        (linux, "linux", "epoll", "linux_observer"),
        (windows, "windows", "iocp", "windows_observer"),
    ):
        require(job, "runs-on: [self-hosted,", workflow)
        require(job, "gamenet-performance", workflow)
        require(job, "git status --porcelain=v1", workflow)
        require(job, "git rev-parse HEAD", workflow)
        require(job, "gamenet_hot_path_benchmark", workflow)
        require(job, "tools/run_hot_path_cost.py", workflow)
        require(job, f"--platform {platform} --backend {backend}", workflow)
        require(job, f"inputs.{observer}", workflow)
        require(job, "--evidence-class fixed-lab --native-runner", workflow)
        require(job, "--working-tree clean", workflow)
        require(job, "--warmups 1 --samples 10", workflow)
        require(job, "tools/validate_hot_path_cost.py", workflow)
        require(job, "--require-fixed-lab", workflow)
        require(job, "uses: actions/upload-artifact@v4", workflow)
        require(job, "if: always()", workflow)
        require(job, "if-no-files-found: error", workflow)
        require(job, "retention-days: 90", workflow)
    require(linux, "taskset -pc $$", workflow)
    require(linux, "lscpu", workflow)
    require(windows, "ProcessorAffinity", workflow)
    require(windows, "Get-CimInstance Win32_Processor", workflow)

    guard = "tests/cmake/test_hot_path_benchmark_contract.py"
    workflow_guard = "tests/ci/test_hot_path_benchmark_workflow.py"
    for source in (ci, long_soak):
        source_text = source.read_text(encoding="utf-8")
        require(source_text, guard, source)
        require(source_text, workflow_guard, source)


if __name__ == "__main__":
    main()
