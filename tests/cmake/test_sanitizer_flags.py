# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path


def check_msvc_tsan_rejection(sanitizer_cmake: Path) -> None:
    # Exercise the CMake branch on every host without requiring an MSVC compiler.
    # The unsupported option must reject before target/compiler commands run.
    with tempfile.TemporaryDirectory(prefix="gamenet-sanitizer-contract-") as directory:
        script = Path(directory) / "unsupported-tsan.cmake"
        script.write_text(
            "set(MSVC TRUE)\n"
            "set(GAMENET_ENABLE_TSAN ON)\n"
            "set(GAMENET_ENABLE_ASAN_UBSAN OFF)\n"
            f'include("{sanitizer_cmake.as_posix()}")\n'
            "gamenet_configure_sanitizers(uncreated_target)\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            ["cmake", "-P", str(script)], check=False, capture_output=True,
            text=True, encoding="utf-8", timeout=30,
        )
        assert result.returncode != 0, "MSVC TSan must fail instead of silently disabling instrumentation"
        assert "GAMENET_ENABLE_TSAN is not supported by MSVC" in result.stdout + result.stderr


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    sanitizer_cmake = repo_root / "cmake" / "Sanitizers.cmake"
    tests_cmake = repo_root / "tests" / "CMakeLists.txt"
    text = sanitizer_cmake.read_text(encoding="utf-8")
    tests_text = tests_cmake.read_text(encoding="utf-8")

    assert "target_compile_options(${target_name} PUBLIC /fsanitize=address)" in text
    assert "target_link_options(${target_name} PUBLIC /fsanitize=address)" not in text
    assert "target_compile_options(${target_name} PUBLIC -fsanitize=address,undefined" in text
    assert "target_link_options(${target_name} PUBLIC -fsanitize=address,undefined)" in text
    assert "target_compile_options(${target_name} PUBLIC -fsanitize=thread" in text
    assert "target_link_options(${target_name} PUBLIC -fsanitize=thread)" in text
    assert "INTERFACE -fsanitize" not in text
    assert "if(MSVC AND GAMENET_ENABLE_ASAN_UBSAN)" in tests_text
    assert "GAMENET_MSVC_ASAN_RUNTIME_DIR" in tests_text
    assert 'PROPERTIES ENVIRONMENT "${GAMENET_TEST_RUNTIME_ENVIRONMENT}"' in tests_text
    check_msvc_tsan_rejection(sanitizer_cmake)


if __name__ == "__main__":
    main()
