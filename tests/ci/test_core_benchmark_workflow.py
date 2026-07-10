from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing core benchmark workflow fragment in {source}: {needle}"


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
    require(text, "--scenario echo", workflow)
    require(text, "--scenario connections", workflow)
    require(text, "--scenario slow-client", workflow)
    require(text, "gamenet.core_benchmark.v1", workflow)
    require(text, "actions/upload-artifact@v4", workflow)
    require(text, "core-benchmark-linux-release-${{ github.sha }}", workflow)
    require(text, "core-benchmark-windows-release-${{ github.sha }}", workflow)
    assert "throughput_mib_per_second" not in text, (
        "manual workflow must validate schema/status, not enforce performance thresholds"
    )

    guard = "tests/ci/test_core_benchmark_workflow.py"
    require(ci_workflow.read_text(encoding="utf-8"), guard, ci_workflow)
    require(long_soak.read_text(encoding="utf-8"), guard, long_soak)

    docs_text = docs.read_text(encoding="utf-8")
    require(docs_text, "core-benchmark", docs)
    require(docs_text, "same commit", docs)
    require(docs_text, "raw JSON artifacts", docs)


if __name__ == "__main__":
    main()
