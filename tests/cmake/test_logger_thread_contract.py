from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing Logger thread-contract fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    intent_index = repo_root / "intents" / "README.md"
    intent = repo_root / "intents" / "modules" / "logger.intent.md"
    rules = repo_root / "rules" / "thread_affinity_rules.md"
    header = repo_root / "include" / "gamenet" / "core" / "base" / "Logger.h"
    source = repo_root / "src" / "core" / "base" / "Logger.cc"
    contract_test = repo_root / "tests" / "contract" / "base" / "test_logger_thread_safety.cpp"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    long_soak = repo_root / ".github" / "workflows" / "long-soak.yml"
    workflow_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"
    long_soak_contract = repo_root / "tests" / "ci" / "test_long_soak_workflow.py"
    ci_docs = repo_root / "docs" / "development" / "ci.md"

    index_text = intent_index.read_text(encoding="utf-8")
    intent_text = intent.read_text(encoding="utf-8")
    rules_text = rules.read_text(encoding="utf-8")
    header_text = header.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    contract_text = contract_test.read_text(encoding="utf-8")
    tests_cmake_text = tests_cmake.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    long_soak_text = long_soak.read_text(encoding="utf-8")
    workflow_contract_text = workflow_contract.read_text(encoding="utf-8")
    long_soak_contract_text = long_soak_contract.read_text(encoding="utf-8")
    ci_docs_text = ci_docs.read_text(encoding="utf-8")

    require(index_text, "intents/modules/logger.intent.md", intent_index)
    require(intent_text, "concurrently from arbitrary threads", intent)
    require(intent_text, "an in-flight snapshot", intent)
    require(intent_text, "callbacks may run concurrently", intent)
    require(intent_text, "resetForTesting()` is only for a quiescent test boundary", intent)
    require(rules_text, "## 8. Logger", rules)

    require(header_text, "Process-global runtime configuration", header)
    require(header_text, "may be called from any thread", header)
    require(header_text, "invoked concurrently", header)
    require(header_text, "quiescent test boundary", header)

    require(source_text, "std::atomic<int> g_logLevel", source)
    require(source_text, "std::mutex g_callbackMutex", source)
    require(source_text, "CallbackSnapshot callbacks()", source)
    require(source_text, "const auto snapshot = callbacks();", source)
    require(source_text, "std::lock_guard<std::mutex> lock(g_callbackMutex);", source)

    require(contract_text, "std::thread reconfigurer", contract_test)
    require(contract_text, "logger-thread-contract", contract_test)
    require(contract_text, "reentrantCallbackCount", contract_test)
    require(contract_text, "expectedRecords", contract_test)
    require(
        tests_cmake_text,
        "test_logger_thread_safety.cpp threading",
        tests_cmake,
    )

    linux_guard = "python3 tests/cmake/test_logger_thread_contract.py"
    windows_guard = "python tests/cmake/test_logger_thread_contract.py"
    require(workflow_text, linux_guard, workflow)
    require(workflow_text, windows_guard, workflow)
    require(long_soak_text, linux_guard, long_soak)
    require(workflow_contract_text, linux_guard, workflow_contract)
    require(long_soak_contract_text, linux_guard, long_soak_contract)
    require(ci_docs_text, "Logger thread-contract guard", ci_docs)


if __name__ == "__main__":
    main()
