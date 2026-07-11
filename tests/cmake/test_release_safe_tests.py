from __future__ import annotations

import re
from pathlib import Path


CPP_ASSERT_PATTERN = re.compile(r"\bassert\s*\(")


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing Release-safe test fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    support_header = repo_root / "tests" / "support" / "TestAssert.h"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"
    ci_docs = repo_root / "docs" / "development" / "ci.md"

    assert support_header.exists(), f"missing Release-safe test helper: {support_header}"

    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    require(tests_cmake_text, "target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})", tests_cmake)

    cpp_files = sorted(
        path
        for path in (repo_root / "tests").rglob("*.cpp")
        if "install_consumer" not in path.relative_to(repo_root).parts
        and "fuzz" not in path.relative_to(repo_root).parts
    )
    assert cpp_files, "no C++ tests found"
    for path in cpp_files:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(repo_root).as_posix()
        assert "#include <cassert>" not in text, f"{rel} still includes <cassert>"
        assert "support/TestAssert.h" in text, f"{rel} does not include Release-safe test helper"
        assert CPP_ASSERT_PATTERN.search(text) is None, f"{rel} still uses standard assert()"

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "python3 tests/cmake/test_release_safe_tests.py", workflow)
    require(workflow_text, "ctest --test-dir build-release --output-on-failure", workflow)

    ci_contract_text = ci_contract.read_text(encoding="utf-8")
    require(ci_contract_text, "python3 tests/cmake/test_release_safe_tests.py", ci_contract)
    require(ci_contract_text, "ctest --test-dir build-release --output-on-failure", ci_contract)
    assert '"ctest --test-dir build-release" not in workflow' not in ci_contract_text

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, "py -3 tests\\cmake\\test_release_safe_tests.py", ci_docs)
    require(ci_docs_text, "ctest --test-dir build-release --output-on-failure", ci_docs)
    assert "Release still builds the test executables, but it does not run CTest" not in ci_docs_text


if __name__ == "__main__":
    main()
