from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing build-governance fragment in {source}: {needle}"


def git(repo_root: Path, *args: str, expected: tuple[int, ...] = (0,)) -> bytes:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        capture_output=True,
        check=False,
    )
    assert result.returncode in expected, (
        f"git {' '.join(args)} failed with {result.returncode}:\n"
        f"{result.stderr.decode('utf-8', errors='replace')}"
    )
    return result.stdout


def verify_m4_preflight(repo_root: Path, license_text: str) -> None:
    manifest_path = (
        repo_root
        / "docs"
        / "development"
        / "m4_external_release_preflight_2026-08-23.json"
    )
    record_path = manifest_path.with_suffix(".md")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    record_text = record_path.read_text(encoding="utf-8")
    normalized_record_text = " ".join(record_text.split())

    assert manifest["schema"] == "gamenet.m4_external_release_preflight.v1"
    assert manifest["status"] == "complete-awaiting-owner-authorization"
    assert manifest["authorization"]["state"] == "pending"
    assert manifest["repository_state"]["visibility"] == "PUBLIC"
    assert manifest["repository_state"]["v0_3_0_tag_present"] is False
    assert manifest["repository_state"]["v0_3_0_release_present"] is False
    assert manifest["license_state"]["classification"] == (
        "all-rights-reserved-no-license-grant"
    )
    assert "All rights reserved." in license_text
    assert "No license is granted" in license_text
    assert not git(repo_root, "tag", "-l", "v0.3.0").strip(), (
        "pre-authorization governance must be updated before creating v0.3.0"
    )

    audit_base = manifest["audit_base"]["commit"]
    audit_tree = manifest["audit_base"]["tree"]
    actual_tree = git(repo_root, "rev-parse", f"{audit_base}^{{tree}}").decode().strip()
    assert actual_tree == audit_tree
    assert git(repo_root, "merge-base", "--is-ancestor", audit_base, "HEAD") == b""

    tree_lines = git(repo_root, "ls-tree", "-r", audit_base).decode().splitlines()
    tracked = manifest["tracked_inventory"]
    assert len(tree_lines) == tracked["total_files"] == 564
    modes = [line.split(maxsplit=1)[0] for line in tree_lines]
    paths = [line.split("\t", maxsplit=1)[1] for line in tree_lines]
    assert modes.count("100644") == tracked["regular_files"] == 564
    assert modes.count("120000") == tracked["symlinks"] == 0
    assert modes.count("160000") == tracked["submodules"] == 0
    extension_counts: dict[str, int] = {}
    for path in paths:
        name = path.rsplit("/", maxsplit=1)[-1]
        extension = f".{name.rsplit('.', maxsplit=1)[-1]}" if "." in name else "<none>"
        extension_counts[extension] = extension_counts.get(extension, 0) + 1
    assert extension_counts == tracked["extensions"]
    assert sum(path == "mini_trantor" or path.startswith("mini_trantor/") for path in paths) == (
        tracked["tracked_mini_trantor_files"]
    )
    archive_or_binary_suffixes = {
        ".7z",
        ".a",
        ".bin",
        ".dat",
        ".dll",
        ".dylib",
        ".exe",
        ".gif",
        ".gz",
        ".jpeg",
        ".jpg",
        ".lib",
        ".pcap",
        ".pdf",
        ".png",
        ".so",
        ".tar",
        ".zip",
    }
    assert sum(
        any(path.lower().endswith(suffix) for suffix in archive_or_binary_suffixes)
        for path in paths
    ) == tracked["tracked_archive_or_native_binary_files"]
    lfs_pointers = git(
        repo_root,
        "grep",
        "-Il",
        "version https://git-lfs.github.com/spec/v1",
        audit_base,
        "--",
        ".",
        expected=(0, 1),
    ).splitlines()
    assert len(lfs_pointers) == tracked["git_lfs_files"] == 0

    license_blob = git(repo_root, "cat-file", "blob", f"{audit_base}:LICENSE")
    assert hashlib.sha256(license_blob).hexdigest() == manifest["license_state"][
        "license_blob_sha256"
    ]
    spdx_files = git(
        repo_root,
        "grep",
        "-Il",
        "SPDX-License-Identifier",
        audit_base,
        "--",
        ".",
        expected=(0, 1),
    ).splitlines()
    copyright_files = git(
        repo_root,
        "grep",
        "-Il",
        "Copyright",
        audit_base,
        "--",
        ".",
        expected=(0, 1),
    ).splitlines()
    assert len(spdx_files) == manifest["license_state"]["spdx_identifier_files"] == 0
    assert len(copyright_files) == manifest["license_state"]["copyright_files"] == 1

    author_lines = sorted(
        set(
            git(repo_root, "log", "--format=%aN <%aE>", audit_base)
            .decode("utf-8")
            .splitlines()
        )
    )
    assert author_lines == sorted(tracked["git_authors"])
    generated = manifest["generated_and_third_party_audit"][
        "deterministic_project_generated_assets"
    ]
    assert len(generated) == 1
    assert generated[0]["generator"] == "tests/fuzz/generate_packet_framer_corpus.py"
    for output in generated[0]["outputs"]:
        content = git(repo_root, "cat-file", "blob", f"{audit_base}:{output['path']}")
        assert len(content) == output["bytes"]
        assert hashlib.sha256(content).hexdigest() == output["sha256"]

    post_m3 = manifest["post_m3_change_audit"]
    assert post_m3["public_surface_changed_since_internal_candidate"] is False
    assert post_m3["fresh_promotion_commit_and_full_matrix_required"] is True
    public_diff = subprocess.run(
        [
            "git",
            "diff",
            "--quiet",
            f"{manifest['audit_base']['internal_candidate_commit']}..{audit_base}",
            "--",
            *post_m3["public_surface_paths"],
        ],
        cwd=repo_root,
        check=False,
    )
    assert public_diff.returncode == 0, "M4 preflight public-surface audit drifted"
    runtime_files = (
        git(
            repo_root,
            "diff",
            "--name-only",
            f"{manifest['audit_base']['internal_candidate_commit']}..{audit_base}",
            "--",
            "src/core/net/TcpConnection.cc",
            "src/core/net/platform/IocpTcpTransport.h",
            "src/core/net/platform/IocpTcpTransport_win.cc",
        )
        .decode("utf-8")
        .splitlines()
    )
    assert runtime_files == post_m3["runtime_files_changed_since_internal_candidate"]

    for fragment in (
        "PREFLIGHT COMPLETE / AWAITING OWNER AUTHORIZATION / NO RELEASE",
        "already a **public** GitHub repository",
        "not a legal opinion",
        "No vendored third-party library",
        "no tracked file contains `SPDX-License-Identifier`",
        "uninterrupted `candidate-1h`, then `release-3h`",
        "Until then, the repository remains all-rights-reserved",
    ):
        require(normalized_record_text, fragment, record_path)


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
    require(
        root_text,
        'if(GAMENET_ENABLE_EXPERIMENTAL AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")',
        root_cmake,
    )
    require(
        root_text,
        "GAMENET_ENABLE_EXPERIMENTAL requires Linux for the IOE-X1 io_uring target",
        root_cmake,
    )
    require(root_text, "add_subdirectory(src/experimental/io_uring)", root_cmake)
    assert root_text.index('project(GameNetCore VERSION 0.3.0 LANGUAGES CXX)') < root_text.index(
        "game-net-core currently supports only Linux and Windows"
    )
    assert root_text.index('option(GAMENET_ENABLE_EXPERIMENTAL "Build experimental modules" OFF)') < (
        root_text.index("if(BUILD_SHARED_LIBS)")
    )
    assert root_text.index("if(GAMENET_ENABLE_EXPERIMENTAL AND NOT") < root_text.index(
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
        "Windows is Tier 2 after completing the IOCP",
        "macOS, BSD variants, and all other target systems fail",
        "`BUILD_SHARED_LIBS=ON` and `GAMENET_ENABLE_TLS=ON`",
        "`GAMENET_ENABLE_EXPERIMENTAL=ON` is supported only on Linux",
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
        "Windows | Tier 2 | IOCP",
        "macOS | Unsupported",
        "FreeBSD, OpenBSD, NetBSD",
        "`BUILD_SHARED_LIBS=ON` is rejected",
        "no binary ABI compatibility promise before version 1.0",
        "`GAMENET_ENABLE_TLS`",
        "`GAMENET_ENABLE_EXPERIMENTAL`",
        "IOE-X1–X10 io_uring",
    ):
        require(docs_text, fragment, platform_docs)

    readme_text = readme.read_text(encoding="utf-8")
    require(readme_text, "## Supported Builds", readme)
    require(readme_text, "Linux/epoll is Tier 1", readme)
    require(readme_text, "Windows/IOCP is a required Tier 2", readme)
    require(readme_text, "static-only before 1.0", readme)
    require(readme_text, "docs/development/platform_support.md", readme)
    require(readme_text, "Linux-only IOE-X1–X10 io_uring", readme)
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
    verify_m4_preflight(repo_root, license_text)

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
