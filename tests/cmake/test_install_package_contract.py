from __future__ import annotations

import json
import re
from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing CMake install/package fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    root_cmake = repo_root / "CMakeLists.txt"
    core_cmake = repo_root / "src" / "core" / "CMakeLists.txt"
    config_template = repo_root / "cmake" / "GameNetCoreConfig.cmake.in"
    consumer_cmake = repo_root / "tests" / "cmake" / "install_consumer" / "CMakeLists.txt"
    consumer_main = repo_root / "tests" / "cmake" / "install_consumer" / "main.cpp"
    provisional_main = (
        repo_root / "tests" / "cmake" / "install_consumer" / "provisional_main.cpp"
    )
    manifest = json.loads(
        (repo_root / "api" / "public_api_manifest.json").read_text(encoding="utf-8")
    )
    workflow = repo_root / ".github" / "workflows" / "ci.yml"

    root_text = root_cmake.read_text(encoding="utf-8")
    core_text = core_cmake.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")

    require(root_text, "include(GNUInstallDirs)", root_cmake)
    require(root_text, "include(CMakePackageConfigHelpers)", root_cmake)
    require(root_text, "GAMENET_INSTALL_CMAKEDIR", root_cmake)
    require(root_text, "configure_package_config_file(", root_cmake)
    require(root_text, "write_basic_package_version_file(", root_cmake)
    require(root_text, "COMPATIBILITY SameMinorVersion", root_cmake)
    require(root_text, "install(DIRECTORY include/gamenet/core", root_cmake)
    for component in ("protocol", "transport", "game_session", "game_logic", "broadcast"):
        require(root_text, f"install(DIRECTORY include/gamenet/{component}", root_cmake)
    require(root_text, "DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet", root_cmake)
    assert "install(DIRECTORY include/gamenet\n" not in root_text, (
        "install package must not copy deferred include/gamenet namespace directories"
    )
    require(root_text, "install(EXPORT GameNetCoreTargets", root_cmake)

    require(core_text, "install(TARGETS gamenet_core", core_cmake)
    require(core_text, "EXPORT_NAME core", core_cmake)
    require(core_text, "EXPORT GameNetCoreTargets", core_cmake)
    require(core_text, "INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}", core_cmake)

    assert config_template.exists(), f"missing package config template: {config_template}"
    config_text = config_template.read_text(encoding="utf-8")
    require(config_text, "@PACKAGE_INIT@", config_template)
    require(config_text, "find_dependency(Threads)", config_template)
    require(config_text, "include(\"${CMAKE_CURRENT_LIST_DIR}/GameNetCoreTargets.cmake\")", config_template)
    require(config_text, "check_required_components(GameNetCore)", config_template)

    assert consumer_cmake.exists(), f"missing install consumer CMake fixture: {consumer_cmake}"
    consumer_cmake_text = consumer_cmake.read_text(encoding="utf-8")
    require(consumer_cmake_text, "find_package(GameNetCore 0.3.0 EXACT REQUIRED)", consumer_cmake)
    require(consumer_cmake_text, "target_link_libraries(gamenet_install_consumer", consumer_cmake)
    core_link = re.search(
        r"target_link_libraries\(gamenet_install_consumer(.*?)\n\)",
        consumer_cmake_text,
        re.DOTALL,
    )
    assert core_link is not None
    require(core_link.group(1), "GameNet::core", consumer_cmake)
    for target in ("protocol", "transport", "game_session", "game_logic", "broadcast"):
        assert f"GameNet::{target}" not in core_link.group(1), (
            "core-only install consumer must not inherit provisional usage requirements"
        )
    require(
        consumer_cmake_text,
        "target_link_libraries(gamenet_provisional_install_consumer",
        consumer_cmake,
    )
    for target in ("protocol", "transport", "game_session", "game_logic", "broadcast"):
        require(consumer_cmake_text, f"GameNet::{target}", consumer_cmake)
    require(consumer_cmake_text, "enable_testing()", consumer_cmake)
    require(
        consumer_cmake_text,
        "add_test(NAME gamenet.install_consumer COMMAND gamenet_install_consumer)",
        consumer_cmake,
    )
    require(consumer_cmake_text, "assert_gamenet_version(0.3.0 TRUE)", consumer_cmake)
    require(consumer_cmake_text, "assert_gamenet_version(0.2.99 FALSE)", consumer_cmake)
    require(consumer_cmake_text, "assert_gamenet_version(0.4.0 FALSE)", consumer_cmake)

    assert consumer_main.exists(), f"missing install consumer source fixture: {consumer_main}"
    consumer_main_text = consumer_main.read_text(encoding="utf-8")
    for header in manifest["headers"]["stable_core"]:
        installed_header = header.removeprefix("include/")
        require(consumer_main_text, f"#include <{installed_header}>", consumer_main)
    for header in manifest["headers"]["provisional"]:
        installed_header = header.removeprefix("include/")
        assert f"#include <{installed_header}>" not in consumer_main_text, (
            f"core-only consumer includes provisional header: {installed_header}"
        )
    require(consumer_main_text, "#include <gamenet/core/net/Buffer.h>", consumer_main)
    require(consumer_main_text, "gamenet::net::Buffer", consumer_main)
    require(consumer_main_text, "#include <gamenet/core/DispatchResult.h>", consumer_main)
    require(consumer_main_text, "gamenet::DispatchResult", consumer_main)
    assert provisional_main.exists(), (
        f"missing provisional install consumer source fixture: {provisional_main}"
    )
    provisional_main_text = provisional_main.read_text(encoding="utf-8")
    require(provisional_main_text, "#include <gamenet/protocol/PacketFramer.h>", provisional_main)
    require(provisional_main_text, "#include <gamenet/transport/TcpTransportEndpoint.h>", provisional_main)
    require(provisional_main_text, "gamenet::transport::TcpTransportEndpoint", provisional_main)
    require(provisional_main_text, "#include <gamenet/game_session/PlayerSession.h>", provisional_main)
    require(provisional_main_text, "gamenet::game_session::PlayerSession", provisional_main)
    require(provisional_main_text, "gamenet::game_logic::GameCommandQueue", provisional_main)
    require(provisional_main_text, "gamenet::broadcast::BroadcastMetric", provisional_main)

    require(workflow_text, "cmake --install build --prefix \"$PWD/build/_install\"", workflow)
    require(workflow_text, "-DCMAKE_PREFIX_PATH=\"$PWD/build/_install\"", workflow)
    require(workflow_text, "cmake -S tests/cmake/install_consumer", workflow)
    require(
        workflow_text,
        "--fail-on-stable-surface-review",
        workflow,
    )
    require(
        workflow_text,
        "--compatibility-output ci-evidence/public-api-compatibility-diff.json",
        workflow,
    )
    assert workflow_text.count("--expected-total 2") == 6, (
        "all six install-consumer inventory commands must cover stable and provisional consumers"
    )
    require(
        workflow_text,
        "ctest --test-dir build-install-consumer --output-on-failure",
        workflow,
    )
    require(
        workflow_text,
        "ctest --test-dir build-windows-install-consumer -C Debug --output-on-failure",
        workflow,
    )
    require(
        workflow_text,
        'cmake --install build-windows-release --config Release --prefix "$pwd/build-windows-release/_install"',
        workflow,
    )
    require(
        workflow_text,
        "cmake -S tests/cmake/install_consumer -B build-windows-release-install-consumer",
        workflow,
    )
    require(
        workflow_text,
        'DCMAKE_PREFIX_PATH="$pwd/build-windows-release/_install"',
        workflow,
    )
    require(
        workflow_text,
        "cmake --build build-windows-release-install-consumer --config Release --parallel",
        workflow,
    )
    require(
        workflow_text,
        "ctest --test-dir build-windows-release-install-consumer -C Release --output-on-failure",
        workflow,
    )


if __name__ == "__main__":
    main()
