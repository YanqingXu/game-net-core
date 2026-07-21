from __future__ import annotations

from pathlib import Path


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


if __name__ == "__main__":
    main()
