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
    require(workflow, "cancel-in-progress: true")
    require(workflow, "python3 tests/cmake/test_install_package_contract.py")
    require(workflow, "-DCMAKE_BUILD_TYPE=Release")
    require(workflow, "-DGAMENET_BUILD_TESTING=ON")
    require(workflow, "-DGAMENET_ENABLE_TLS=OFF")
    require(workflow, "-DGAMENET_ENABLE_EXPERIMENTAL=OFF")
    require(workflow, "cmake --install build --prefix \"$PWD/build/_install\"")
    require(workflow, "cmake -S tests/cmake/install_consumer")
    require(workflow, "-DCMAKE_PREFIX_PATH=\"$PWD/build/_install\"")
    assert "ctest --test-dir build-release" not in workflow


if __name__ == "__main__":
    main()
