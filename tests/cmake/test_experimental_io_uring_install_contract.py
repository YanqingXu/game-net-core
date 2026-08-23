# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import re
from pathlib import Path


PUBLIC_HEADERS = {
    "IoUringCompletionEngine.h",
    "IoUringEventLoopPump.h",
    "IoUringTcpClient.h",
    "IoUringTcpConnectionAdapter.h",
    "IoUringTcpConnectionHub.h",
    "IoUringTcpServer.h",
}


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing IOE-X15 install fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    root_cmake = repo_root / "CMakeLists.txt"
    target_cmake = repo_root / "src" / "experimental" / "io_uring" / "CMakeLists.txt"
    config_template = repo_root / "cmake" / "GameNetCoreConfig.cmake.in"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    benchmark_cmake = repo_root / "benchmarks" / "CMakeLists.txt"
    consumer_dir = repo_root / "tests" / "cmake" / "experimental_io_uring_install_consumer"
    consumer_cmake = consumer_dir / "CMakeLists.txt"
    consumer_main = consumer_dir / "main.cpp"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"
    version_note = repo_root / "docs" / "development" / "releases" / "v0.5.0-experimental-preview.md"
    intent = repo_root / "intents" / "modules" / "io_uring_experimental_package.intent.md"
    stable_manifest = json.loads(
        (repo_root / "api" / "public_api_manifest.json").read_text(encoding="utf-8")
    )

    root_text = root_cmake.read_text(encoding="utf-8")
    require(root_text, 'option(GAMENET_ENABLE_EXPERIMENTAL "Build experimental modules" OFF)', root_cmake)
    require(root_text, "GAMENET_ENABLE_EXPERIMENTAL AND NOT CMAKE_SYSTEM_NAME STREQUAL \"Linux\"", root_cmake)
    require(root_text, "add_subdirectory(src/experimental/io_uring)", root_cmake)

    target_text = target_cmake.read_text(encoding="utf-8")
    require(target_text, "add_library(gamenet_experimental_io_uring STATIC", target_cmake)
    require(target_text, "add_library(GameNet::experimental_io_uring ALIAS", target_cmake)
    require(target_text, "EXPORT_NAME experimental_io_uring", target_cmake)
    require(target_text, "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>", target_cmake)
    require(target_text, "$<INSTALL_INTERFACE:include>", target_cmake)
    require(target_text, "PUBLIC GAMENET_EXPERIMENTAL_IO_URING=1", target_cmake)
    require(target_text, "install(TARGETS gamenet_experimental_io_uring", target_cmake)
    require(target_text, "EXPORT GameNetCoreTargets", target_cmake)
    require(target_text, "include/gamenet/experimental/io_uring", target_cmake)
    assert re.search(r"GameNet::experimental(?!_io_uring)", target_text) is None

    public_root = repo_root / "include" / "gamenet" / "experimental" / "io_uring"
    assert {path.name for path in public_root.glob("*.h")} == PUBLIC_HEADERS
    source_root = repo_root / "src" / "experimental" / "io_uring"
    for public_header in PUBLIC_HEADERS:
        assert not (source_root / public_header).exists()
    assert (source_root / "IoUringTcpConnectionDriver.h").is_file()
    assert (source_root / "IoUringTcpMultiOwnerServer.h").is_file()

    config_text = config_template.read_text(encoding="utf-8")
    require(config_text, 'set(GameNetCore_EXPERIMENTAL_IO_URING_API_VERSION "0.1.0")', config_template)
    require(config_text, "set(GameNetCore_experimental_io_uring_FOUND FALSE)", config_template)
    require(config_text, "if(TARGET GameNet::experimental_io_uring)", config_template)
    require(config_text, "set(GameNetCore_experimental_io_uring_FOUND TRUE)", config_template)
    require(config_text, "GameNetCore_FIND_REQUIRED_experimental_io_uring", config_template)
    require(config_text, "Required component experimental_io_uring is unavailable", config_template)
    require(config_text, "check_required_components(GameNetCore)", config_template)

    consumer_cmake_text = consumer_cmake.read_text(encoding="utf-8")
    require(consumer_cmake_text, "COMPONENTS experimental_io_uring", consumer_cmake)
    require(consumer_cmake_text, "GameNet::experimental_io_uring", consumer_cmake)
    require(consumer_cmake_text, "gamenet.experimental_io_uring_install_consumer", consumer_cmake)
    consumer_text = consumer_main.read_text(encoding="utf-8")
    require(consumer_text, "<gamenet/experimental/io_uring/IoUringTcpServer.h>", consumer_main)
    require(consumer_text, "<gamenet/experimental/io_uring/IoUringTcpClient.h>", consumer_main)
    require(consumer_text, "GAMENET_EXPERIMENTAL_IO_URING", consumer_main)

    for cmake_path in (tests_cmake, benchmark_cmake):
        cmake_text = cmake_path.read_text(encoding="utf-8")
        require(cmake_text, "GameNet::experimental_io_uring", cmake_path)
        assert re.search(r"GameNet::experimental(?!_io_uring)", cmake_text) is None

    workflow_text = workflow.read_text(encoding="utf-8")
    require(workflow_text, "Install and verify experimental io_uring package consumer", workflow)
    require(workflow_text, "cmake --install build-io-uring-experimental", workflow)
    require(workflow_text, "tests/cmake/experimental_io_uring_install_consumer", workflow)
    require(workflow_text, "experimental-io-uring-install-consumer-inventory.json", workflow)
    require(workflow_text, "experimental-io-uring-install-consumer-junit.xml", workflow)

    intent_text = intent.read_text(encoding="utf-8")
    require(intent_text, "target: GameNet::experimental_io_uring", intent)
    require(intent_text, "find_package(GameNetCore REQUIRED COMPONENTS experimental_io_uring)", intent)
    require(version_note.read_text(encoding="utf-8"), "no tag or GitHub Release", version_note)

    stable_targets = [target for values in stable_manifest["targets"].values() for target in values]
    stable_headers = [header for values in stable_manifest["headers"].values() for header in values]
    assert "GameNet::experimental_io_uring" not in stable_targets
    assert not any("/experimental/" in header for header in stable_headers)


if __name__ == "__main__":
    main()
