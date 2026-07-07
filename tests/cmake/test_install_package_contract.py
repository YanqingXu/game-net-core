from __future__ import annotations

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
    workflow = repo_root / ".github" / "workflows" / "ci.yml"

    root_text = root_cmake.read_text(encoding="utf-8")
    core_text = core_cmake.read_text(encoding="utf-8")
    workflow_text = workflow.read_text(encoding="utf-8")

    require(root_text, "include(GNUInstallDirs)", root_cmake)
    require(root_text, "include(CMakePackageConfigHelpers)", root_cmake)
    require(root_text, "GAMENET_INSTALL_CMAKEDIR", root_cmake)
    require(root_text, "configure_package_config_file(", root_cmake)
    require(root_text, "write_basic_package_version_file(", root_cmake)
    require(root_text, "install(DIRECTORY include/gamenet/core", root_cmake)
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
    require(consumer_cmake_text, "find_package(GameNetCore REQUIRED)", consumer_cmake)
    require(consumer_cmake_text, "target_link_libraries(gamenet_install_consumer PRIVATE GameNet::core)", consumer_cmake)

    assert consumer_main.exists(), f"missing install consumer source fixture: {consumer_main}"
    consumer_main_text = consumer_main.read_text(encoding="utf-8")
    require(consumer_main_text, "#include <gamenet/core/net/Buffer.h>", consumer_main)
    require(consumer_main_text, "gamenet::net::Buffer", consumer_main)

    require(workflow_text, "cmake --install build --prefix \"$PWD/build/_install\"", workflow)
    require(workflow_text, "-DCMAKE_PREFIX_PATH=\"$PWD/build/_install\"", workflow)
    require(workflow_text, "cmake -S tests/cmake/install_consumer", workflow)


if __name__ == "__main__":
    main()
