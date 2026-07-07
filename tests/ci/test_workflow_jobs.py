from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str) -> None:
    assert needle in text, f"missing workflow fragment: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    workflow = (repo_root / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")

    require(workflow, "linux-cmake:")
    require(workflow, "Linux CMake build and tests")
    require(workflow, "linux-asan-ubsan:")
    require(workflow, "Linux ASan/UBSan build and tests")
    require(workflow, "linux-release:")
    require(workflow, "Linux Release build")
    require(workflow, "windows-msvc:")
    require(workflow, "Windows MSVC IOCP build and tests")
    require(workflow, "runs-on: windows-latest")
    require(workflow, "cancel-in-progress: true")
    require(workflow, "python3 tests/scope/test_intent_consistency.py")
    require(workflow, "python3 tests/cmake/test_install_package_contract.py")
    require(workflow, "python3 tests/cmake/test_migration_status_contract.py")
    require(workflow, "python3 tests/cmake/test_msvc_utf8_contract.py")
    require(workflow, "python3 tests/cmake/test_platform_backend_contract.py")
    require(workflow, "python3 tests/cmake/test_tcp_lifecycle_contracts.py")
    require(workflow, "python3 tests/cmake/test_tcp_connection_context_contract.py")
    require(workflow, "python3 tests/cmake/test_windows_iocp_milestone_contract.py")
    require(workflow, "python3 tests/cmake/test_windows_iocp_data_path_contract.py")
    require(workflow, "python3 tests/cmake/test_release_safe_tests.py")
    require(workflow, "-DCMAKE_BUILD_TYPE=Release")
    require(workflow, "-DGAMENET_BUILD_TESTING=ON")
    require(workflow, "-DGAMENET_ENABLE_TLS=OFF")
    require(workflow, "-DGAMENET_ENABLE_EXPERIMENTAL=OFF")
    require(workflow, "cmake --install build --prefix \"$PWD/build/_install\"")
    require(workflow, "cmake -S tests/cmake/install_consumer")
    require(workflow, "-DCMAKE_PREFIX_PATH=\"$PWD/build/_install\"")
    require(workflow, "ctest --test-dir build-release --output-on-failure")
    require(workflow, "-G \"Visual Studio 18 2026\"")
    require(workflow, "-A x64")
    require(workflow, "cmake --build build-windows --config Debug --parallel")
    require(workflow, "ctest --test-dir build-windows -C Debug --output-on-failure --timeout 10")
    require(workflow, "cmake --install build-windows --config Debug --prefix \"$pwd/build-windows/_install\"")
    require(workflow, "cmake -S tests/cmake/install_consumer -B build-windows-install-consumer")
    require(workflow, "-DCMAKE_PREFIX_PATH=\"$pwd/build-windows/_install\"")
    require(workflow, "cmake --build build-windows-install-consumer --config Debug --parallel")


if __name__ == "__main__":
    main()
