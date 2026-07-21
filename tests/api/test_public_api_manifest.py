from __future__ import annotations

import copy
import importlib.util
import json
import tempfile
from pathlib import Path


def load_verifier(repo_root: Path):
    verifier_path = repo_root / "tools" / "verify_public_api_manifest.py"
    spec = importlib.util.spec_from_file_location("verify_public_api_manifest", verifier_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    manifest_path = repo_root / "api" / "public_api_manifest.json"
    verifier = load_verifier(repo_root)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    errors = verifier.verify_manifest(repo_root, manifest_path)
    assert not errors, "\n".join(errors)

    with tempfile.TemporaryDirectory(prefix="gamenet-api-manifest-") as directory:
        temporary = Path(directory) / "manifest.json"

        missing_header = copy.deepcopy(manifest)
        removed = missing_header["headers"]["stable_core"].pop()
        missing_header["stable_header_fingerprints"].pop(removed)
        temporary.write_text(json.dumps(missing_header), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("public header inventory mismatch" in error for error in errors)

        modified_surface = copy.deepcopy(manifest)
        stable_header = modified_surface["headers"]["stable_core"][0]
        modified_surface["stable_header_fingerprints"][stable_header] = "0" * 64
        temporary.write_text(json.dumps(modified_surface), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("stable public surface changed" in error for error in errors)

        wrong_version = copy.deepcopy(manifest)
        wrong_version["package_version"] = "99.0.0"
        temporary.write_text(json.dumps(wrong_version), encoding="utf-8")
        errors = verifier.verify_manifest(repo_root, temporary)
        assert any("package_version must match CMake" in error for error in errors)

    normalized = verifier.normalize_cpp_public_surface(
        'int value; // comment\nconst char* url = "https://example.invalid/a"; /* block */\n'
    )
    assert normalized == 'int value; const char* url = "https://example.invalid/a";'

    print("public API manifest contract verified")


if __name__ == "__main__":
    main()
