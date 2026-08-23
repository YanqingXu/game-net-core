# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Sequence

from assemble_release import (
    artifact_record,
    files_from_directory,
    resolve_commit,
    validate_project_metadata,
    write_tar_gz,
    write_zip,
)


V02_ASSET_NAME = "game-net-core-v0.2.0-phase4-preview.tar.gz"
V02_ASSET_SHA256 = "c2f5f2ece4a147f63f91c86ef3c1dd25bf9d370d22e25889648132985f9af408"
V02_TAG = "v0.2.0-phase4-preview"
V02_COMMIT = "7668d6b82a0d815ccd79f83c572bc0a36bcceea0"
V02_ROOT = "game-net-core-v0.2.0-phase4-preview"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_member(name: str) -> PurePosixPath:
    normalized = name.replace("\\", "/")
    path = PurePosixPath(normalized)
    if (
        not normalized
        or path.is_absolute()
        or ".." in path.parts
        or re.match(r"^[A-Za-z]:", normalized)
    ):
        raise ValueError(f"unsafe archive member: {name!r}")
    return path


def extract_archive(archive_path: Path, destination: Path) -> str:
    if destination.exists():
        raise FileExistsError(f"refusing to overwrite extraction directory: {destination}")
    destination.mkdir(parents=True)
    roots: set[str] = set()
    try:
        if archive_path.name.endswith(".tar.gz"):
            with tarfile.open(archive_path, mode="r:gz") as archive:
                members = archive.getmembers()
                for member in members:
                    path = _safe_member(member.name)
                    roots.add(path.parts[0])
                    if member.isdir():
                        continue
                    if not member.isfile():
                        raise ValueError(f"archive contains non-file entry: {member.name}")
                    source = archive.extractfile(member)
                    if source is None:
                        raise ValueError(f"cannot read archive member: {member.name}")
                    target = destination.joinpath(*path.parts)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(source.read())
                    if os.name != "nt":
                        target.chmod(member.mode & 0o777)
        elif archive_path.suffix == ".zip":
            with zipfile.ZipFile(archive_path) as archive:
                for info in archive.infolist():
                    path = _safe_member(info.filename)
                    roots.add(path.parts[0])
                    if info.is_dir():
                        continue
                    target = destination.joinpath(*path.parts)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.read(info))
                    if os.name != "nt":
                        target.chmod((info.external_attr >> 16) & 0o777)
        else:
            raise ValueError(f"unsupported archive format: {archive_path}")
    except BaseException:
        shutil.rmtree(destination, ignore_errors=True)
        raise
    if len(roots) != 1:
        shutil.rmtree(destination, ignore_errors=True)
        raise ValueError(f"archive must contain exactly one root directory: {roots}")
    return next(iter(roots))


class CommandRecorder:
    def __init__(self, evidence_dir: Path) -> None:
        self.evidence_dir = evidence_dir
        self.commands: list[list[str]] = []
        self.logs: list[dict[str, object]] = []

    def run(self, name: str, command: Sequence[str], cwd: Path) -> None:
        command_list = [str(item) for item in command]
        result = subprocess.run(
            command_list,
            cwd=cwd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        log_path = self.evidence_dir / f"{name}.log"
        log_path.write_text(
            f"command: {' '.join(command_list)}\n"
            f"exit_code: {result.returncode}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}\n",
            encoding="utf-8",
            newline="\n",
        )
        self.commands.append(command_list)
        self.logs.append(
            {
                "path": log_path.name,
                "sha256": _sha256(log_path),
                "exit_code": result.returncode,
            }
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"consumer gate command failed ({result.returncode}): {' '.join(command_list)}\n"
                f"{result.stdout}\n{result.stderr}"
            )


def _generator_arguments(args: argparse.Namespace) -> list[str]:
    result: list[str] = []
    if args.generator:
        result.extend(["-G", args.generator])
    if args.architecture:
        result.extend(["-A", args.architecture])
    return result


def _configure_build_install_v02(
    args: argparse.Namespace,
    recorder: CommandRecorder,
    source: Path,
    build: Path,
    install: Path,
) -> None:
    recorder.run(
        "v02-configure",
        [
            args.cmake,
            "-S",
            str(source),
            "-B",
            str(build),
            *_generator_arguments(args),
            f"-DCMAKE_BUILD_TYPE={args.configuration}",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DGAMENET_BUILD_TESTING=OFF",
            "-DGAMENET_BUILD_BENCHMARKS=OFF",
            "-DGAMENET_BUILD_FUZZING=OFF",
            "-DGAMENET_ENABLE_TLS=OFF",
            "-DGAMENET_ENABLE_EXPERIMENTAL=OFF",
        ],
        args.repo_root,
    )
    recorder.run(
        "v02-build",
        [args.cmake, "--build", str(build), "--config", args.configuration, "--parallel"],
        args.repo_root,
    )
    recorder.run(
        "v02-install",
        [
            args.cmake,
            "--install",
            str(build),
            "--config",
            args.configuration,
            "--prefix",
            str(install),
        ],
        args.repo_root,
    )


def _run_consumer(
    args: argparse.Namespace,
    recorder: CommandRecorder,
    *,
    name: str,
    source: Path,
    build: Path,
    prefix: Path,
    expected_tests: int,
    expected_version: str | None = None,
) -> dict[str, object]:
    configure = [
        args.cmake,
        "-S",
        str(source),
        "-B",
        str(build),
        *_generator_arguments(args),
        f"-DCMAKE_BUILD_TYPE={args.configuration}",
        f"-DCMAKE_PREFIX_PATH={prefix}",
    ]
    if expected_version:
        configure.append(f"-DGAMENET_EXPECTED_VERSION={expected_version}")
    recorder.run(f"{name}-configure", configure, args.repo_root)
    inventory_path = args.evidence_dir / f"{name}-inventory.json"
    recorder.run(
        f"{name}-inventory",
        [
            args.python,
            str(args.repo_root / "tools" / "verify_ctest_inventory.py"),
            "--test-dir",
            str(build),
            "--config",
            args.configuration,
            "--expected-total",
            str(expected_tests),
            "--output",
            str(inventory_path),
        ],
        args.repo_root,
    )
    recorder.run(
        f"{name}-build",
        [args.cmake, "--build", str(build), "--config", args.configuration, "--parallel"],
        args.repo_root,
    )
    junit_path = args.evidence_dir / f"{name}-junit.xml"
    ctest_log = args.evidence_dir / f"{name}-ctest.log"
    recorder.run(
        f"{name}-test",
        [
            args.ctest,
            "--test-dir",
            str(build),
            "-C",
            args.configuration,
            "--output-on-failure",
            "--timeout",
            "60",
            "--output-junit",
            str(junit_path),
            "--output-log",
            str(ctest_log),
        ],
        args.repo_root,
    )
    return {
        "name": name,
        "version": expected_version or "0.3.0",
        "tests": f"{expected_tests}/{expected_tests}",
        "status": "success",
        "inventory": inventory_path.name,
        "inventory_sha256": _sha256(inventory_path),
        "junit": junit_path.name,
        "junit_sha256": _sha256(junit_path),
        "ctest_log": ctest_log.name,
        "ctest_log_sha256": _sha256(ctest_log),
    }


def run_gate(args: argparse.Namespace) -> None:
    args.repo_root = args.repo_root.resolve()
    args.current_install_tree = args.current_install_tree.resolve()
    args.v02_source_archive = args.v02_source_archive.resolve()
    args.work_root = args.work_root.resolve()
    args.evidence_dir = args.evidence_dir.resolve()
    if args.work_root.exists():
        raise FileExistsError(f"refusing to overwrite work root: {args.work_root}")
    args.work_root.mkdir(parents=True)
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    if any(args.evidence_dir.iterdir()):
        raise FileExistsError(f"evidence directory must be empty: {args.evidence_dir}")

    commit, tree, epoch = resolve_commit(args.repo_root, args.candidate_sha)
    if commit != args.candidate_sha or args.candidate_sha != resolve_commit(args.repo_root, "HEAD")[0]:
        raise ValueError("consumer gate candidate must be the exact checked-out HEAD commit")
    if args.platform == "linux":
        package_kind = "linux-x86_64"
        archive_suffix = ".tar.gz"
        backend = "epoll"
    else:
        package_kind = "windows-x86_64"
        archive_suffix = ".zip"
        backend = "iocp"

    install_files = files_from_directory(args.current_install_tree)
    validate_project_metadata(install_files, "0.3.0")
    package_root = f"game-net-core-v0.3.0-{package_kind}"
    package_path = args.work_root / f"game-net-core-v0.3.0-{package_kind}{archive_suffix}"
    if archive_suffix == ".zip":
        write_zip(package_path, install_files, package_root)
    else:
        write_tar_gz(package_path, install_files, package_root, epoch)
    package = artifact_record(package_kind, package_path, len(install_files))
    retained_package = args.evidence_dir / package.name
    shutil.copy2(package_path, retained_package)
    if _sha256(retained_package) != package.sha256:
        raise ValueError("retained current package hash mismatch")

    extracted_v03 = args.work_root / "extracted-v03"
    root = extract_archive(package_path, extracted_v03)
    if root != package_root:
        raise ValueError(f"current package root mismatch: {root}")
    current_prefix = extracted_v03 / root
    extracted_files = files_from_directory(current_prefix)
    if {item.path: item.data for item in extracted_files} != {
        item.path: item.data for item in install_files
    }:
        raise ValueError("extracted current package differs from the staged install tree")
    validate_project_metadata(extracted_files, "0.3.0")

    if args.v02_source_archive.name != V02_ASSET_NAME:
        raise ValueError(f"v0.2 source asset must be named {V02_ASSET_NAME}")
    if _sha256(args.v02_source_archive) != V02_ASSET_SHA256:
        raise ValueError("v0.2 canonical source archive SHA256 mismatch")
    extracted_v02 = args.work_root / "extracted-v02"
    v02_root = extract_archive(args.v02_source_archive, extracted_v02)
    if v02_root != V02_ROOT:
        raise ValueError(f"v0.2 source archive root mismatch: {v02_root}")

    recorder = CommandRecorder(args.evidence_dir)
    v02_install = args.work_root / "v02-install"
    _configure_build_install_v02(
        args,
        recorder,
        extracted_v02 / v02_root,
        args.work_root / "v02-build",
        v02_install,
    )
    consumers = [
        _run_consumer(
            args,
            recorder,
            name="current-extracted",
            source=args.repo_root / "tests" / "cmake" / "install_consumer",
            build=args.work_root / "current-extracted-consumer",
            prefix=current_prefix,
            expected_tests=2,
        ),
        _run_consumer(
            args,
            recorder,
            name="upgrade-v02",
            source=args.repo_root / "tests" / "cmake" / "upgrade_consumer",
            build=args.work_root / "upgrade-v02-consumer",
            prefix=v02_install,
            expected_tests=1,
            expected_version="0.2.0",
        ),
        _run_consumer(
            args,
            recorder,
            name="upgrade-v03",
            source=args.repo_root / "tests" / "cmake" / "upgrade_consumer",
            build=args.work_root / "upgrade-v03-consumer",
            prefix=current_prefix,
            expected_tests=1,
            expected_version="0.3.0",
        ),
    ]
    manifest = {
        "schema": "gamenet.release_consumer_evidence.v1",
        "status": "success",
        "candidate_sha": commit,
        "source_tree_sha1": tree,
        "platform": args.platform,
        "backend": backend,
        "configuration": args.configuration,
        "current_package": dataclasses.asdict(package),
        "v02_source": {
            "tag": V02_TAG,
            "commit": V02_COMMIT,
            "asset": V02_ASSET_NAME,
            "sha256": V02_ASSET_SHA256,
        },
        "consumers": consumers,
        "commands": recorder.commands,
        "command_logs": recorder.logs,
    }
    output = args.evidence_dir / "manifest.json"
    output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(
        f"validated {args.platform}/{backend} extracted package and v0.2-to-v0.3 "
        f"upgrade consumers for {commit}"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the extracted-package and v0.2-to-v0.3 release consumer gate."
    )
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--current-install-tree", type=Path, required=True)
    parser.add_argument("--v02-source-archive", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--generator")
    parser.add_argument("--architecture")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--python", default=os.fspath(Path(sys.executable)))
    return parser


def main() -> None:
    run_gate(build_parser().parse_args())


if __name__ == "__main__":
    main()
