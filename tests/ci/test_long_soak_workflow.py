from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing long-soak workflow fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    workflow = repo_root / ".github" / "workflows" / "long-soak.yml"
    ci_workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    migration_status = repo_root / "docs" / "migration_status.md"

    assert workflow.exists(), f"missing non-default long-soak workflow: {workflow}"

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "name: long-soak", workflow)
    require(workflow_text, "workflow_dispatch:", workflow)
    assert "\n  push:" not in workflow_text, "long-soak must not run on push"
    assert "\n  pull_request:" not in workflow_text, "long-soak must not run on pull_request"
    require(workflow_text, "linux-long-soak:", workflow)
    require(workflow_text, "Linux long-soak threading contracts", workflow)
    require(workflow_text, "runs-on: ubuntu-24.04", workflow)
    require(workflow_text, "-DGAMENET_BUILD_TESTING=ON", workflow)
    require(workflow_text, "-DGAMENET_ENABLE_TLS=OFF", workflow)
    require(workflow_text, "-DGAMENET_ENABLE_EXPERIMENTAL=OFF", workflow)
    require(workflow_text, "python3 tests/cmake/test_event_loop_contracts.py", workflow)
    require(workflow_text, "ctest --test-dir build-long-soak --output-on-failure", workflow)
    require(workflow_text, "-L threading", workflow)
    require(workflow_text, "--repeat until-fail:${{ inputs.repeat }}", workflow)
    require(workflow_text, "--timeout ${{ inputs.timeout_seconds }}", workflow)

    ci_workflow_text = ci_workflow.read_text(encoding="utf-8")
    require(ci_workflow_text, "python3 tests/ci/test_long_soak_workflow.py", ci_workflow)
    require(ci_workflow_text, "python tests/ci/test_long_soak_workflow.py", ci_workflow)

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, "test_long_soak_workflow.py", ci_docs)
    require(ci_docs_text, "Long-soak repository guards include `tests/cmake/test_event_loop_contracts.py`", ci_docs)
    require(ci_docs_text, "long-soak", ci_docs)
    require(ci_docs_text, "ctest --test-dir build-long-soak --output-on-failure", ci_docs)
    require(ci_docs_text, "--repeat until-fail:", ci_docs)
    require(ci_docs_text, "Remote evidence: run 28986707243", ci_docs)
    require(ci_docs_text, "job 86017363504", ci_docs)
    require(ci_docs_text, "commit 9b27a0a3c3993cb1f90ef4357fa80027205ca221", ci_docs)
    require(ci_docs_text, "repeat 20", ci_docs)
    require(ci_docs_text, "timeout 60 seconds", ci_docs)
    require(ci_docs_text, "36/36 threading-labeled tests passed", ci_docs)
    require(ci_docs_text, "previous 43-test threading slice", ci_docs)
    require(ci_docs_text, "expanded 44-test threading slice", ci_docs)

    migration_text = migration_status.read_text(encoding="utf-8")
    require(migration_text, "non-default `long-soak` workflow", migration_status)
    require(migration_text, "long-soak repository guard parity includes the EventLoop contract guard", migration_status)
    require(migration_text, "`ctest --repeat until-fail`", migration_status)
    require(migration_text, "Remote GitHub `long-soak` evidence is now recorded", migration_status)


if __name__ == "__main__":
    main()
