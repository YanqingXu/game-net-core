# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence


LICENSE_ID = "Apache-2.0"
COPYRIGHT_TEXT = "Copyright 2026 Yanqing Xu"
PROJECT_URL = "https://github.com/YanqingXu/game-net-core"
SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".py",
    ".cmake",
    ".in",
    ".sh",
    ".ps1",
    ".yml",
    ".yaml",
}


@dataclasses.dataclass(frozen=True)
class ReleaseFile:
    path: str
    data: bytes
    mode: int = 0o644


@dataclasses.dataclass(frozen=True)
class Artifact:
    kind: str
    name: str
    sha256: str
    bytes: int
    file_count: int


def _run(repo_root: Path, *args: str) -> bytes:
    result = subprocess.run(
        list(args),
        cwd=repo_root,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(args)}\n"
            f"{result.stderr.decode('utf-8', errors='replace')}"
        )
    return result.stdout


def _git(repo_root: Path, *args: str) -> bytes:
    return _run(repo_root, "git", *args)


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode(
        "utf-8"
    )


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _safe_relative(path: str) -> str:
    normalized = path.replace("\\", "/")
    pure = PurePosixPath(normalized)
    if (
        not normalized
        or pure.is_absolute()
        or ".." in pure.parts
        or normalized.startswith("/")
        or re.match(r"^[A-Za-z]:", normalized)
    ):
        raise ValueError(f"unsafe release path: {path!r}")
    return pure.as_posix()


def _source_header_required(path: str) -> bool:
    pure = PurePosixPath(path)
    return pure.name == "CMakeLists.txt" or pure.suffix.lower() in SOURCE_SUFFIXES


def resolve_commit(repo_root: Path, revision: str) -> tuple[str, str, int]:
    commit = _git(repo_root, "rev-parse", "--verify", f"{revision}^{{commit}}").decode().strip()
    tree = _git(repo_root, "rev-parse", f"{commit}^{{tree}}").decode().strip()
    epoch = int(_git(repo_root, "show", "-s", "--format=%ct", commit).decode().strip())
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError(f"resolved commit is not a full SHA-1: {commit}")
    if not re.fullmatch(r"[0-9a-f]{40}", tree):
        raise ValueError(f"resolved tree is not a full SHA-1: {tree}")
    return commit, tree, epoch


def source_files_from_git(repo_root: Path, commit: str, archive_root: str) -> list[ReleaseFile]:
    archive = _git(
        repo_root,
        "archive",
        "--format=tar",
        f"--prefix={archive_root}/",
        commit,
    )
    files: list[ReleaseFile] = []
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as source_tar:
        for member in source_tar.getmembers():
            if member.isdir():
                continue
            if not member.isfile():
                raise ValueError(f"source tree contains unsupported archive entry: {member.name}")
            relative = member.name.removeprefix(f"{archive_root}/")
            relative = _safe_relative(relative)
            extracted = source_tar.extractfile(member)
            if extracted is None:
                raise ValueError(f"could not read source archive entry: {member.name}")
            files.append(ReleaseFile(relative, extracted.read(), member.mode & 0o777))
    return sorted(files, key=lambda item: item.path)


def files_from_directory(root: Path) -> list[ReleaseFile]:
    if not root.is_dir():
        raise ValueError(f"release input is not a directory: {root}")
    files: list[ReleaseFile] = []
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        if path.is_symlink():
            raise ValueError(f"symlinks are not supported in release inputs: {path}")
        if not path.is_file():
            continue
        relative = _safe_relative(path.relative_to(root).as_posix())
        mode = 0o755 if os.access(path, os.X_OK) else 0o644
        files.append(ReleaseFile(relative, path.read_bytes(), mode))
    if not files:
        raise ValueError(f"release input directory is empty: {root}")
    return files


def validate_project_metadata(files: Sequence[ReleaseFile], version: str) -> None:
    by_path = {item.path: item.data for item in files}
    required = ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md")
    is_source = "CMakeLists.txt" in by_path
    licensing: dict[str, bytes] = {}
    for name in required:
        candidates = [
            data
            for path, data in by_path.items()
            if path == name
            or path == f"share/doc/GameNetCore/{name}"
            or path.endswith(f"/share/doc/GameNetCore/{name}")
        ]
        if len(candidates) == 1:
            licensing[name] = candidates[0]
    missing = [path for path in required if path not in licensing]
    if missing:
        raise ValueError(f"release input is missing licensing metadata: {missing}")
    license_text = licensing["LICENSE"].decode("utf-8")
    if "Apache License" not in license_text or "Version 2.0, January 2004" not in license_text:
        raise ValueError("LICENSE is not the canonical Apache License 2.0 text")
    if COPYRIGHT_TEXT not in licensing["NOTICE"].decode("utf-8"):
        raise ValueError("NOTICE does not contain the project copyright attribution")
    if LICENSE_ID not in licensing["THIRD_PARTY_NOTICES.md"].decode("utf-8"):
        raise ValueError("THIRD_PARTY_NOTICES.md does not declare Apache-2.0")

    if is_source:
        cmake = by_path["CMakeLists.txt"].decode("utf-8")
        if f"project(GameNetCore VERSION {version} LANGUAGES CXX)" not in cmake:
            raise ValueError("CMake project version does not match the release version")
        api = json.loads(by_path["api/public_api_manifest.json"])
        if api.get("package_version") != version or api.get("package_license") != LICENSE_ID:
            raise ValueError("public API manifest version/license does not match release metadata")
        release_metadata = json.loads(by_path[f"release/v{version}.json"])
        if (
            release_metadata.get("version") != version
            or release_metadata.get("name") != f"v{version}"
            or release_metadata.get("license") != LICENSE_ID
        ):
            raise ValueError("release metadata version/name/license is inconsistent")
        missing_headers = [
            item.path
            for item in files
            if _source_header_required(item.path)
            and (
                b"SPDX-License-Identifier: Apache-2.0" not in item.data
                or COPYRIGHT_TEXT.encode("utf-8") not in item.data
            )
        ]
        if missing_headers:
            raise ValueError(f"source files missing canonical SPDX headers: {missing_headers[:10]}")
    else:
        config_candidates = [
            item.data
            for item in files
            if item.path.endswith("GameNetCoreConfig.cmake")
        ]
        if len(config_candidates) != 1 or b'GameNetCore_LICENSE "Apache-2.0"' not in config_candidates[0]:
            raise ValueError("installed package does not export GameNetCore_LICENSE=Apache-2.0")


def _canonical_tar(files: Sequence[ReleaseFile], archive_root: str, epoch: int) -> bytes:
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:", format=tarfile.GNU_FORMAT) as archive:
        for item in sorted(files, key=lambda entry: entry.path):
            info = tarfile.TarInfo(f"{archive_root}/{_safe_relative(item.path)}")
            info.size = len(item.data)
            info.mode = item.mode
            info.mtime = epoch
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            archive.addfile(info, io.BytesIO(item.data))
    return output.getvalue()


def write_tar_gz(
    output: Path,
    files: Sequence[ReleaseFile],
    archive_root: str,
    epoch: int,
) -> None:
    tar_bytes = _canonical_tar(files, archive_root, epoch)
    with output.open("wb") as raw:
        with gzip.GzipFile(
            filename="",
            mode="wb",
            compresslevel=9,
            fileobj=raw,
            mtime=0,
        ) as compressed:
            compressed.write(tar_bytes)


def write_zip(output: Path, files: Sequence[ReleaseFile], archive_root: str) -> None:
    with zipfile.ZipFile(
        output,
        mode="w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
        strict_timestamps=True,
    ) as archive:
        for item in sorted(files, key=lambda entry: entry.path):
            info = zipfile.ZipInfo(
                f"{archive_root}/{_safe_relative(item.path)}",
                date_time=(1980, 1, 1, 0, 0, 0),
            )
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | item.mode) << 16
            archive.writestr(info, item.data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def artifact_record(kind: str, path: Path, file_count: int) -> Artifact:
    data = path.read_bytes()
    return Artifact(kind, path.name, _sha256(data), len(data), file_count)


def _spdx_id(prefix: str, path: str) -> str:
    digest = hashlib.sha256(f"{prefix}\0{path}".encode("utf-8")).hexdigest()[:24]
    return f"SPDXRef-{prefix}-{digest}"


def _verification_code(files: Sequence[ReleaseFile]) -> str:
    sha1_values = sorted(hashlib.sha1(item.data, usedforsecurity=False).hexdigest() for item in files)
    return hashlib.sha1("".join(sha1_values).encode("ascii"), usedforsecurity=False).hexdigest()


def create_spdx_document(
    *,
    version: str,
    commit: str,
    tree: str,
    created: str,
    source_files: Sequence[ReleaseFile],
    binary_sets: Sequence[tuple[str, str, Sequence[ReleaseFile], Artifact]],
    source_artifact: Artifact,
) -> dict[str, object]:
    packages: list[dict[str, object]] = []
    spdx_files: list[dict[str, object]] = []
    relationships: list[dict[str, str]] = []

    def add_package(
        package_key: str,
        display_name: str,
        package_files: Sequence[ReleaseFile],
        artifact: Artifact,
    ) -> None:
        package_id = f"SPDXRef-Package-{package_key}"
        packages.append(
            {
                "SPDXID": package_id,
                "name": display_name,
                "versionInfo": version,
                "packageFileName": artifact.name,
                "downloadLocation": (
                    f"{PROJECT_URL}/releases/download/v{version}/{artifact.name}"
                ),
                "filesAnalyzed": True,
                "packageVerificationCode": {
                    "packageVerificationCodeValue": _verification_code(package_files)
                },
                "checksums": [{"algorithm": "SHA256", "checksumValue": artifact.sha256}],
                "homepage": PROJECT_URL,
                "sourceInfo": f"Git commit {commit}; tree {tree}",
                "licenseConcluded": LICENSE_ID,
                "licenseDeclared": LICENSE_ID,
                "copyrightText": COPYRIGHT_TEXT,
                "externalRefs": [
                    {
                        "referenceCategory": "PACKAGE-MANAGER",
                        "referenceType": "purl",
                        "referenceLocator": f"pkg:github/YanqingXu/game-net-core@{version}",
                    }
                ],
            }
        )
        relationships.append(
            {
                "spdxElementId": "SPDXRef-DOCUMENT",
                "relationshipType": "DESCRIBES",
                "relatedSpdxElement": package_id,
            }
        )
        for item in package_files:
            file_id = _spdx_id(f"File-{package_key}", item.path)
            spdx_files.append(
                {
                    "SPDXID": file_id,
                    "fileName": f"./{item.path}",
                    "checksums": [
                        {
                            "algorithm": "SHA1",
                            "checksumValue": hashlib.sha1(
                                item.data, usedforsecurity=False
                            ).hexdigest(),
                        },
                        {"algorithm": "SHA256", "checksumValue": _sha256(item.data)},
                    ],
                    "licenseConcluded": LICENSE_ID,
                    "licenseInfoInFiles": [LICENSE_ID],
                    "copyrightText": COPYRIGHT_TEXT,
                }
            )
            relationships.append(
                {
                    "spdxElementId": package_id,
                    "relationshipType": "CONTAINS",
                    "relatedSpdxElement": file_id,
                }
            )

    add_package("Source", "game-net-core source", source_files, source_artifact)
    for package_key, display_name, package_files, artifact in binary_sets:
        add_package(package_key, display_name, package_files, artifact)

    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"game-net-core-v{version}",
        "documentNamespace": f"{PROJECT_URL}/spdx/v{version}/{commit}",
        "creationInfo": {
            "created": created,
            "creators": ["Tool: game-net-core/tools/assemble_release.py"],
            "comment": "Generated deterministically from the immutable release commit and staged install trees.",
        },
        "documentDescribes": [package["SPDXID"] for package in packages],
        "packages": packages,
        "files": spdx_files,
        "relationships": relationships,
    }


def parse_labeled_paths(values: Sequence[str], option: str) -> list[tuple[str, Path]]:
    parsed: list[tuple[str, Path]] = []
    labels: set[str] = set()
    for value in values:
        label, separator, raw_path = value.partition("=")
        if not separator or not re.fullmatch(r"[a-z0-9][a-z0-9._-]*", label):
            raise ValueError(f"{option} must use label=path with a safe lowercase label: {value}")
        if label in labels:
            raise ValueError(f"duplicate {option} label: {label}")
        labels.add(label)
        parsed.append((label, Path(raw_path).resolve()))
    return parsed


def collect_evidence(inputs: Sequence[tuple[str, Path]]) -> tuple[list[ReleaseFile], list[dict[str, object]]]:
    packaged: list[ReleaseFile] = []
    index: list[dict[str, object]] = []
    for label, root in sorted(inputs):
        for item in files_from_directory(root):
            path = f"{label}/{item.path}"
            packaged.append(ReleaseFile(path, item.data, item.mode))
            index.append(
                {
                    "path": path,
                    "bytes": len(item.data),
                    "sha256": _sha256(item.data),
                }
            )
    return packaged, sorted(index, key=lambda item: str(item["path"]))


def validate_promotion_manifest(
    path: Path,
    evidence_inputs: Sequence[tuple[str, Path]],
    commit: str,
) -> tuple[str, str]:
    resolved = path.resolve()
    if not resolved.is_file():
        raise ValueError(f"promotion manifest is not a file: {resolved}")
    packaged_path = ""
    for label, root in evidence_inputs:
        try:
            relative = resolved.relative_to(root)
        except ValueError:
            continue
        packaged_path = f"{label}/{_safe_relative(relative.as_posix())}"
        break
    if not packaged_path:
        raise ValueError("--promotion-manifest must be contained by one --evidence input")
    document = json.loads(resolved.read_text(encoding="utf-8"))
    if (
        document.get("schema") != "gamenet.production_promotion_evidence.v2"
        or document.get("stage") != "release"
        or document.get("status") != "success"
        or document.get("candidate_sha") != commit
    ):
        raise ValueError(
            "promotion manifest must be a successful release-stage v2 record "
            "for the exact assembled commit"
        )
    capacity = document.get("capacity", {})
    if capacity.get("profile") != "dedicated-100k" or capacity.get("endpoint_attempts") != 100000:
        raise ValueError("release promotion must bind the dedicated-100k capacity profile")
    durations = {
        item.get("mode"): item.get("elapsed_milliseconds", 0)
        for item in document.get("endurance", [])
    }
    if durations.get("candidate-1h", 0) < 3_600_000 or durations.get("release-3h", 0) < 10_800_000:
        raise ValueError("release promotion must contain completed candidate-1h and release-3h evidence")
    return packaged_path, _sha256(resolved.read_bytes())


def assemble(args: argparse.Namespace) -> Path:
    repo_root = args.repo_root.resolve()
    commit, tree, commit_epoch = resolve_commit(repo_root, args.commit)
    if args.expected_commit and commit != args.expected_commit:
        raise ValueError(f"resolved commit {commit} does not match --expected-commit {args.expected_commit}")
    epoch = args.source_date_epoch if args.source_date_epoch is not None else commit_epoch
    created = dt.datetime.fromtimestamp(epoch, tz=dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    version = args.version
    release_name = f"v{version}"
    archive_root = f"game-net-core-{release_name}"

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing release directory: {output}")
    temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=output.parent))
    artifacts_dir = temporary / "artifacts"
    artifacts_dir.mkdir()

    try:
        source_files = source_files_from_git(repo_root, commit, archive_root)
        validate_project_metadata(source_files, version)
        source_tar = artifacts_dir / f"game-net-core-{release_name}-source.tar.gz"
        source_zip = artifacts_dir / f"game-net-core-{release_name}-source.zip"
        write_tar_gz(source_tar, source_files, archive_root, epoch)
        write_zip(source_zip, source_files, archive_root)
        artifacts = [
            artifact_record("source-tar", source_tar, len(source_files)),
            artifact_record("source-zip", source_zip, len(source_files)),
        ]

        binary_sets: list[tuple[str, str, Sequence[ReleaseFile], Artifact]] = []
        for platform, root, kind, suffix in (
            ("Linux", args.linux_install_tree, "linux-x86_64", ".tar.gz"),
            ("Windows", args.windows_install_tree, "windows-x86_64", ".zip"),
        ):
            package_files = files_from_directory(root.resolve())
            validate_project_metadata(package_files, version)
            package_root = f"{archive_root}-{kind}"
            package_path = artifacts_dir / f"game-net-core-{release_name}-{kind}{suffix}"
            if suffix == ".zip":
                write_zip(package_path, package_files, package_root)
            else:
                write_tar_gz(package_path, package_files, package_root, epoch)
            record = artifact_record(kind, package_path, len(package_files))
            artifacts.append(record)
            binary_sets.append((platform, f"game-net-core {platform} x86_64", package_files, record))

        evidence_inputs = parse_labeled_paths(args.evidence, "--evidence")
        if not evidence_inputs:
            raise ValueError("at least one --evidence label=path input is required")
        promotion_path, promotion_sha256 = validate_promotion_manifest(
            args.promotion_manifest, evidence_inputs, commit
        )
        evidence_files, evidence_entries = collect_evidence(evidence_inputs)
        evidence_root = f"{archive_root}-evidence"
        evidence_zip = artifacts_dir / f"game-net-core-{release_name}-evidence.zip"
        write_zip(evidence_zip, evidence_files, evidence_root)
        artifacts.append(artifact_record("evidence", evidence_zip, len(evidence_files)))

        source_by_path = {item.path: item.data for item in source_files}
        for name in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
            (temporary / name).write_bytes(source_by_path[name])

        evidence_index = {
            "schema": "gamenet.release_evidence_index.v1",
            "release": release_name,
            "version": version,
            "candidate_sha": commit,
            "source_tree_sha1": tree,
            "generated_at_utc": created,
            "evidence_archive": evidence_zip.name,
            "source_roots": [label for label, _ in sorted(evidence_inputs)],
            "promotion_manifest": {
                "path": promotion_path,
                "sha256": promotion_sha256,
            },
            "file_count": len(evidence_entries),
            "evidence_files": evidence_entries,
        }
        (temporary / "evidence-index.json").write_bytes(_json_bytes(evidence_index))

        spdx = create_spdx_document(
            version=version,
            commit=commit,
            tree=tree,
            created=created,
            source_files=source_files,
            binary_sets=binary_sets,
            source_artifact=artifacts[0],
        )
        (temporary / "sbom.spdx.json").write_bytes(_json_bytes(spdx))

        release_metadata = json.loads(source_by_path[f"release/v{version}.json"])
        package_build = {
            "schema": "gamenet.external_release_package.v1",
            "name": release_name,
            "version": version,
            "license": LICENSE_ID,
            "external_release": True,
            "candidate_sha": commit,
            "source_tree_sha1": tree,
            "source_date_epoch": epoch,
            "generated_at_utc": created,
            "support": release_metadata["support"],
            "known_limitations": release_metadata["known_limitations"],
            "artifacts": [dataclasses.asdict(item) for item in artifacts],
        }
        (temporary / "package-build.json").write_bytes(_json_bytes(package_build))

        checksum_paths = [
            *[Path("artifacts") / item.name for item in artifacts],
            Path("LICENSE"),
            Path("NOTICE"),
            Path("THIRD_PARTY_NOTICES.md"),
            Path("evidence-index.json"),
            Path("package-build.json"),
            Path("sbom.spdx.json"),
        ]
        checksum_lines = []
        for relative in sorted(checksum_paths, key=lambda item: item.as_posix()):
            checksum_lines.append(
                f"{_sha256((temporary / relative).read_bytes())}  {relative.as_posix()}"
            )
        (temporary / "SHA256SUMS").write_text(
            "\n".join(checksum_lines) + "\n", encoding="utf-8", newline="\n"
        )
        temporary.replace(output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return output


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Assemble deterministic game-net-core external release assets."
    )
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--commit", default="HEAD")
    parser.add_argument("--expected-commit")
    parser.add_argument("--version", default="0.3.0")
    parser.add_argument("--source-date-epoch", type=int)
    parser.add_argument("--linux-install-tree", type=Path, required=True)
    parser.add_argument("--windows-install-tree", type=Path, required=True)
    parser.add_argument(
        "--evidence",
        action="append",
        default=[],
        metavar="LABEL=PATH",
        help="Add an evidence directory under a stable lowercase label (repeatable).",
    )
    parser.add_argument(
        "--promotion-manifest",
        type=Path,
        required=True,
        help="Successful same-commit release-stage promotion manifest within an evidence input.",
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser


def main() -> None:
    args = build_parser().parse_args()
    output = assemble(args)
    print(f"assembled deterministic release bundle: {output}")


if __name__ == "__main__":
    main()
