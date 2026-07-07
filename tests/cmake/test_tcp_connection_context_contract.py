from __future__ import annotations

import re
from pathlib import Path


def require(condition: bool, message: str) -> None:
    assert condition, message


def function_body(text: str, signature: str, source: Path) -> str:
    match = re.search(r"(?m)^" + re.escape(signature) + r"\s*\{", text)
    require(match is not None, f"missing function in {source}: {signature}")
    brace_start = match.end() - 1

    depth = 0
    for index in range(brace_start, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace_start + 1 : index]

    raise AssertionError(f"unterminated function body in {source}: {signature}")


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    header = repo_root / "include" / "gamenet" / "core" / "net" / "TcpConnection.h"
    source = repo_root / "src" / "core" / "net" / "TcpConnection.cc"
    contract_test = repo_root / "tests" / "contract" / "tcp_connection" / "test_tcp_connection_lifecycle.cpp"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    ci_contract = repo_root / "tests" / "ci" / "test_workflow_jobs.py"

    header_text = header.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    contract_text = contract_test.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")
    ci_contract_text = ci_contract.read_text(encoding="utf-8")

    require(
        "Connection context is loop-owned mutable state" in header_text,
        "TcpConnection.h must document owner-loop-only context access",
    )
    require(
        "setContext(), getContext()" in header_text,
        "TcpConnection.h must name the context accessors in the thread contract",
    )

    for signature in (
        "void TcpConnection::setContext(std::any context)",
        "const std::any& TcpConnection::getContext() const",
        "std::any& TcpConnection::getContext()",
    ):
        body = function_body(source_text, signature, source)
        require(
            "loop_->assertInLoopThread();" in body,
            f"{signature} must assert owner EventLoop thread before touching context_",
        )

    require(
        "owner-loop-context" in contract_text,
        "TcpConnection contract test must cover context access on the owner loop",
    )
    require(
        "python3 tests/cmake/test_tcp_connection_context_contract.py" in workflow_text,
        "CI workflow must run the TcpConnection context contract guard",
    )
    require(
        "python3 tests/cmake/test_tcp_connection_context_contract.py" in ci_contract_text,
        "workflow contract test must require the TcpConnection context contract guard",
    )


if __name__ == "__main__":
    main()
