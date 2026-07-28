from __future__ import annotations

import re
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing build-governance fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    root_cmake = repo_root / "CMakeLists.txt"
    core_cmake = repo_root / "src" / "core" / "CMakeLists.txt"
    installed_target_files = {
        "gamenet_core": core_cmake,
        "gamenet_protocol": repo_root / "src" / "protocol" / "CMakeLists.txt",
        "gamenet_transport": repo_root / "src" / "transport" / "CMakeLists.txt",
        "gamenet_game_session": repo_root / "src" / "game_session" / "CMakeLists.txt",
        "gamenet_game_logic": repo_root / "src" / "game_logic" / "CMakeLists.txt",
        "gamenet_broadcast": repo_root / "src" / "broadcast" / "CMakeLists.txt",
    }
    platform_intent = repo_root / "intents" / "modules" / "platform_runtime.intent.md"
    release_intent = repo_root / "intents" / "usecases" / "production_candidate_release.intent.md"
    platform_docs = repo_root / "docs" / "development" / "platform_support.md"
    licensing_docs = repo_root / "docs" / "development" / "licensing.md"
    license_file = repo_root / "LICENSE"
    ci_docs = repo_root / "docs" / "development" / "ci.md"
    readme = repo_root / "README.md"
    ci_workflow = repo_root / ".github" / "workflows" / "ci.yml"
    soak_workflow = repo_root / ".github" / "workflows" / "long-soak.yml"
    guard_command_linux = "python3 tests/cmake/test_build_governance_contract.py"
    guard_command_windows = "python tests/cmake/test_build_governance_contract.py"

    root_text = root_cmake.read_text(encoding="utf-8")
    require(root_text, 'CMAKE_SYSTEM_NAME STREQUAL "Linux"', root_cmake)
    require(root_text, 'CMAKE_SYSTEM_NAME STREQUAL "Windows"', root_cmake)
    require(
        root_text,
        "game-net-core currently supports only Linux and Windows",
        root_cmake,
    )
    require(root_text, "if(BUILD_SHARED_LIBS)", root_cmake)
    require(root_text, "game-net-core is static-only before 1.0", root_cmake)
    require(root_text, "if(GAMENET_ENABLE_TLS)", root_cmake)
    require(root_text, "GAMENET_ENABLE_TLS=ON is not implemented", root_cmake)
    require(root_text, "if(GAMENET_ENABLE_EXPERIMENTAL)", root_cmake)
    require(
        root_text,
        "GAMENET_ENABLE_EXPERIMENTAL=ON is not implemented",
        root_cmake,
    )
    assert root_text.index('project(GameNetCore VERSION 0.3.0 LANGUAGES CXX)') < root_text.index(
        "game-net-core currently supports only Linux and Windows"
    )
    assert root_text.index('option(GAMENET_ENABLE_EXPERIMENTAL "Build experimental modules" OFF)') < (
        root_text.index("if(BUILD_SHARED_LIBS)")
    )
    assert root_text.index("if(GAMENET_ENABLE_EXPERIMENTAL)") < root_text.index(
        "add_subdirectory(src/core)"
    )

    core_text = core_cmake.read_text(encoding="utf-8")
    platform_selection = re.search(
        r'if\(CMAKE_SYSTEM_NAME STREQUAL "Windows"\)(?P<body>.*?)'
        r'elseif\(CMAKE_SYSTEM_NAME STREQUAL "Linux"\)(?P<linux>.*?)'
        r'else\(\)(?P<unsupported>.*?)endif\(\)',
        core_text,
        flags=re.DOTALL,
    )
    assert platform_selection is not None, (
        "GameNet::core must have explicit Windows, Linux, and unsupported branches"
    )
    require(platform_selection.group("body"), "net/poller/IocpPoller.cc", core_cmake)
    require(platform_selection.group("linux"), "net/poller/EPollPoller.cc", core_cmake)
    require(
        platform_selection.group("unsupported"),
        "GameNet::core has no backend for CMAKE_SYSTEM_NAME=",
        core_cmake,
    )
    assert "EPollPoller.cc" not in platform_selection.group("body")
    assert "IocpPoller.cc" not in platform_selection.group("linux")

    for target, cmake_file in installed_target_files.items():
        cmake_text = cmake_file.read_text(encoding="utf-8")
        assert re.search(rf"add_library\(\s*{re.escape(target)}\s+STATIC\b", cmake_text), (
            f"installed target must be explicitly static: {target} in {cmake_file}"
        )

    intent_text = platform_intent.read_text(encoding="utf-8")
    for fragment in (
        "The configured target system must be exactly Linux or Windows.",
        "Linux is the Tier 1 reference platform",
        "Windows is Tier 2 until the M3 IOCP",
        "macOS, BSD variants, and all other target systems fail",
        "`BUILD_SHARED_LIBS=ON`, `GAMENET_ENABLE_TLS=ON`, and",
        "No binary ABI compatibility is promised before version 1.0.",
    ):
        require(intent_text, fragment, platform_intent)

    release_text = release_intent.read_text(encoding="utf-8")
    require(release_text, "candidate library targets are static-only", release_intent)
    require(release_text, "Linux is the Tier 1 release-evidence platform", release_intent)
    require(release_text, "macOS, BSD variants, other target systems", release_intent)

    docs_text = platform_docs.read_text(encoding="utf-8")
    for fragment in (
        "Linux | Tier 1 | epoll",
        "Windows | Tier 2 until M3 | IOCP",
        "macOS | Unsupported",
        "FreeBSD, OpenBSD, NetBSD",
        "`BUILD_SHARED_LIBS=ON` is rejected",
        "no binary ABI compatibility promise before version 1.0",
        "`GAMENET_ENABLE_TLS`",
        "`GAMENET_ENABLE_EXPERIMENTAL`",
    ):
        require(docs_text, fragment, platform_docs)

    readme_text = readme.read_text(encoding="utf-8")
    require(readme_text, "## Supported Builds", readme)
    require(readme_text, "Linux/epoll is Tier 1", readme)
    require(readme_text, "Windows/IOCP is a required Tier 2", readme)
    require(readme_text, "static-only before 1.0", readme)
    require(readme_text, "docs/development/platform_support.md", readme)
    require(readme_text, "## Licensing Status", readme)
    require(readme_text, "all-rights-reserved", readme)
    require(readme_text, "docs/development/licensing.md", readme)

    license_text = license_file.read_text(encoding="utf-8")
    require(license_text, "All rights reserved.", license_file)
    require(license_text, "No license is granted", license_file)
    licensing_text = licensing_docs.read_text(encoding="utf-8")
    require(licensing_text, "all-rights-reserved", licensing_docs)
    require(licensing_text, "blocked until the project owner", licensing_docs)
    require(licensing_text, "SPDX identifiers", licensing_docs)

    ci_docs_text = ci_docs.read_text(encoding="utf-8")
    require(ci_docs_text, guard_command_linux, ci_docs)
    require(ci_docs_text, "platform_support.md", ci_docs)

    ci_workflow_text = ci_workflow.read_text(encoding="utf-8")
    assert ci_workflow_text.count(guard_command_linux) == 4, (
        "all four Linux main-CI producers must run the build-governance guard"
    )
    assert ci_workflow_text.count(guard_command_windows) == 2, (
        "both Windows main-CI producers must run the build-governance guard"
    )

    soak_workflow_text = soak_workflow.read_text(encoding="utf-8")
    assert soak_workflow_text.count(guard_command_linux) == 2, (
        "the long-soak repeat and self-hosted CI jobs must each run the "
        "build-governance guard"
    )


if __name__ == "__main__":
    main()
