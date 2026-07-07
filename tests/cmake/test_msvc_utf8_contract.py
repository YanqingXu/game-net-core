from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing MSVC build contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    core_cmake = repo_root / "src" / "core" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"
    ci_docs = repo_root / "docs" / "development" / "ci.md"

    core_text = core_cmake.read_text(encoding="utf-8")
    require(core_text, "target_compile_options(gamenet_core", core_cmake)
    require(core_text, "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>", core_cmake)
    require(core_text, "PUBLIC", core_cmake)
    require(core_text, "$<$<CXX_COMPILER_ID:MSVC>:/FS>", core_cmake)
    require(core_text, "PRIVATE", core_cmake)

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "python3 tests/cmake/test_msvc_utf8_contract.py", workflow)

    ci_contract_text = ci_contract.read_text(encoding="utf-8")
    require(ci_contract_text, "python3 tests/cmake/test_msvc_utf8_contract.py", ci_contract)

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, "test_msvc_utf8_contract.py", ci_docs)
    require(ci_docs_text, "MSVC", ci_docs)
    require(ci_docs_text, "/utf-8", ci_docs)
    require(ci_docs_text, "/FS", ci_docs)


if __name__ == "__main__":
    main()
