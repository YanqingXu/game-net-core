# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HEADER = "# Copyright 2026 Yanqing Xu\n# SPDX-License-Identifier: Apache-2.0\n\n"
LICENSE = "Apache License\nVersion 2.0, January 2004\n"
NOTICE = "game-net-core\nCopyright 2026 Yanqing Xu\n"
THIRD_PARTY = "No bundled third-party dependencies. Project license: Apache-2.0.\n"


def run(command: list[str], cwd: Path, *, expected: int = 0, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    assert result.returncode == expected, (
        f"command returned {result.returncode}, expected {expected}: {' '.join(command)}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    return result


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def create_source_repository(root: Path) -> str:
    write_text(root / "LICENSE", LICENSE)
    write_text(root / "NOTICE", NOTICE)
    write_text(root / "THIRD_PARTY_NOTICES.md", THIRD_PARTY)
    write_text(
        root / "CMakeLists.txt",
        HEADER + "cmake_minimum_required(VERSION 3.20)\nproject(GameNetCore VERSION 0.3.0 LANGUAGES CXX)\n",
    )
    write_text(
        root / "api" / "public_api_manifest.json",
        '{"package_version":"0.3.0","package_license":"Apache-2.0"}\n',
    )
    write_text(
        root / "release" / "v0.3.0.json",
        '{"schema":"gamenet.release_metadata.v1","name":"v0.3.0",'
        '"version":"0.3.0","license":"Apache-2.0",'
        '"support":{"linux-x86_64":{"tier":1},"windows-x86_64":{"tier":2}},'
        '"known_limitations":["fixture limitation"]}\n',
    )
    write_text(root / "src" / "fixture.cpp", "// Copyright 2026 Yanqing Xu\n// SPDX-License-Identifier: Apache-2.0\n\nint fixture = 0;\n")
    write_text(root / "tools" / "fixture.py", HEADER + "print('fixture')\n")

    run(["git", "init", "-b", "main"], root)
    run(["git", "config", "core.autocrlf", "false"], root)
    run(["git", "add", "--all"], root)
    environment = os.environ.copy()
    environment.update(
        {
            "GIT_AUTHOR_NAME": "Release Fixture",
            "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
            "GIT_AUTHOR_DATE": "2026-08-23T00:00:00Z",
            "GIT_COMMITTER_NAME": "Release Fixture",
            "GIT_COMMITTER_EMAIL": "fixture@example.invalid",
            "GIT_COMMITTER_DATE": "2026-08-23T00:00:00Z",
        }
    )
    run(["git", "-c", "commit.gpgSign=false", "commit", "-m", "fixture"], root, env=environment)
    return run(["git", "rev-parse", "HEAD"], root).stdout.strip()


def create_install_tree(root: Path, platform: str) -> None:
    write_text(root / "share" / "doc" / "GameNetCore" / "LICENSE", LICENSE)
    write_text(root / "share" / "doc" / "GameNetCore" / "NOTICE", NOTICE)
    write_text(
        root / "share" / "doc" / "GameNetCore" / "THIRD_PARTY_NOTICES.md",
        THIRD_PARTY,
    )
    write_text(
        root / "lib" / "cmake" / "GameNetCore" / "GameNetCoreConfig.cmake",
        'set(GameNetCore_LICENSE "Apache-2.0")\n',
    )
    write_text(root / "include" / "gamenet" / "fixture.hpp", f"fixture-{platform}\n")
    (root / "lib").mkdir(parents=True, exist_ok=True)
    (root / "lib" / ("GameNetCore.lib" if platform == "windows" else "libGameNetCore.a")).write_bytes(
        f"binary-{platform}".encode("ascii")
    )


def directory_hashes(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    assembler = repo_root / "tools" / "assemble_release.py"
    verifier = repo_root / "tools" / "verify_release_bundle.py"
    schema_value = os.environ.get("GAMENET_TEST_SPDX_SCHEMA", "")
    schema_arguments = ["--spdx-schema", str(Path(schema_value).resolve())] if schema_value else []
    with tempfile.TemporaryDirectory(prefix="gamenet-release-assembler-") as temporary:
        work = Path(temporary)
        source = work / "source"
        source.mkdir()
        commit = create_source_repository(source)
        linux_install = work / "linux-install"
        windows_install = work / "windows-install"
        create_install_tree(linux_install, "linux")
        create_install_tree(windows_install, "windows")
        evidence = work / "evidence"
        write_text(
            evidence / "promotion.json",
            '{"schema":"gamenet.production_promotion_evidence.v2",'
            f'"candidate_sha":"{commit}","stage":"release","status":"success",'
            '"capacity":{"profile":"dedicated-100k","endpoint_attempts":100000},'
            '"endurance":[{"mode":"candidate-1h","elapsed_milliseconds":3600000},'
            '{"mode":"release-3h","elapsed_milliseconds":10800000}]}\n',
        )
        write_text(evidence / "ci" / "manifest.json", f'{{"candidate_sha":"{commit}","tests":"passed"}}\n')

        outputs = [work / "bundle-a", work / "bundle-b"]
        for output in outputs:
            run(
                [
                    sys.executable,
                    str(assembler),
                    "--repo-root",
                    str(source),
                    "--commit",
                    commit,
                    "--expected-commit",
                    commit,
                    "--linux-install-tree",
                    str(linux_install),
                    "--windows-install-tree",
                    str(windows_install),
                    "--evidence",
                    f"promotion={evidence}",
                    "--promotion-manifest",
                    str(evidence / "promotion.json"),
                    "--output",
                    str(output),
                ],
                repo_root,
            )
            run(
                [
                    sys.executable,
                    str(verifier),
                    "--bundle",
                    str(output),
                    "--expected-commit",
                    commit,
                    *schema_arguments,
                ],
                repo_root,
            )

        assert directory_hashes(outputs[0]) == directory_hashes(outputs[1]), (
            "identical immutable inputs must produce byte-identical release bundles"
        )

        write_text(
            evidence / "wrong-promotion.json",
            '{"schema":"gamenet.production_promotion_evidence.v2",'
            '"candidate_sha":"0000000000000000000000000000000000000000",'
            '"stage":"release","status":"success",'
            '"capacity":{"profile":"dedicated-100k","endpoint_attempts":100000},'
            '"endurance":[{"mode":"candidate-1h","elapsed_milliseconds":3600000},'
            '{"mode":"release-3h","elapsed_milliseconds":10800000}]}\n',
        )
        rejected = subprocess.run(
            [
                sys.executable,
                str(assembler),
                "--repo-root",
                str(source),
                "--commit",
                commit,
                "--linux-install-tree",
                str(linux_install),
                "--windows-install-tree",
                str(windows_install),
                "--evidence",
                f"promotion={evidence}",
                "--promotion-manifest",
                str(evidence / "wrong-promotion.json"),
                "--output",
                str(work / "rejected-bundle"),
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
        assert rejected.returncode != 0, "a cross-commit promotion manifest must be rejected"

        tampered = work / "tampered"
        shutil.copytree(outputs[0], tampered)
        with (tampered / "NOTICE").open("ab") as stream:
            stream.write(b"tamper\n")
        failed = subprocess.run(
            [
                sys.executable,
                str(verifier),
                "--bundle",
                str(tampered),
                "--expected-commit",
                commit,
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
        assert failed.returncode != 0, "tampered release metadata must fail verification"

    print("validated deterministic external release assembly and tamper rejection")


if __name__ == "__main__":
    main()
