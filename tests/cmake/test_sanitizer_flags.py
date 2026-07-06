from __future__ import annotations

from pathlib import Path


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    sanitizer_cmake = repo_root / "cmake" / "Sanitizers.cmake"
    text = sanitizer_cmake.read_text(encoding="utf-8")

    assert "target_compile_options(${target_name} PUBLIC /fsanitize=address)" in text
    assert "target_link_options(${target_name} PUBLIC /fsanitize=address)" in text
    assert "target_compile_options(${target_name} PUBLIC -fsanitize=address,undefined" in text
    assert "target_link_options(${target_name} PUBLIC -fsanitize=address,undefined)" in text
    assert "target_compile_options(${target_name} PUBLIC -fsanitize=thread" in text
    assert "target_link_options(${target_name} PUBLIC -fsanitize=thread)" in text
    assert "INTERFACE -fsanitize" not in text


if __name__ == "__main__":
    main()
