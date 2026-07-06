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
    require(workflow, "Linux Release build and tests")
    require(workflow, "-DCMAKE_BUILD_TYPE=Release")
    require(workflow, "-DGAMENET_BUILD_TESTING=ON")
    require(workflow, "-DGAMENET_ENABLE_TLS=OFF")
    require(workflow, "-DGAMENET_ENABLE_EXPERIMENTAL=OFF")
    require(workflow, "ctest --test-dir build-release --output-on-failure")


if __name__ == "__main__":
    main()
