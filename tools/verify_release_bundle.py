# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tarfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Sequence

from assemble_release import LICENSE_ID, validate_project_metadata
from assemble_release import ReleaseFile as ReleaseFile


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_member(name: str) -> str:
    normalized = name.replace("\\", "/")
    pure = PurePosixPath(normalized)
    if pure.is_absolute() or ".." in pure.parts or re.match(r"^[A-Za-z]:", normalized):
        raise ValueError(f"unsafe archive member: {name}")
    return pure.as_posix()


def read_tar(path: Path) -> tuple[str, list[ReleaseFile]]:
    entries: list[ReleaseFile] = []
    roots: set[str] = set()
    with tarfile.open(path, mode="r:gz") as archive:
        for member in archive.getmembers():
            name = safe_member(member.name)
            roots.add(PurePosixPath(name).parts[0])
            if not member.isfile():
                raise ValueError(f"release tar contains non-file member: {name}")
            extracted = archive.extractfile(member)
            if extracted is None:
                raise ValueError(f"cannot read archive member: {name}")
            relative = PurePosixPath(*PurePosixPath(name).parts[1:]).as_posix()
            entries.append(ReleaseFile(relative, extracted.read(), member.mode & 0o777))
    if len(roots) != 1:
        raise ValueError(f"archive must contain one root directory: {path}")
    return next(iter(roots)), sorted(entries, key=lambda item: item.path)


def read_zip(path: Path) -> tuple[str, list[ReleaseFile]]:
    entries: list[ReleaseFile] = []
    roots: set[str] = set()
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            name = safe_member(info.filename)
            if info.is_dir():
                raise ValueError(f"release zip contains an unexpected directory entry: {name}")
            roots.add(PurePosixPath(name).parts[0])
            relative = PurePosixPath(*PurePosixPath(name).parts[1:]).as_posix()
            entries.append(ReleaseFile(relative, archive.read(info), (info.external_attr >> 16) & 0o777))
    if len(roots) != 1:
        raise ValueError(f"archive must contain one root directory: {path}")
    return next(iter(roots)), sorted(entries, key=lambda item: item.path)


def file_map(files: Sequence[ReleaseFile]) -> dict[str, bytes]:
    result: dict[str, bytes] = {}
    for item in files:
        if item.path in result:
            raise ValueError(f"duplicate archive member: {item.path}")
        result[item.path] = item.data
    return result


def load_checksums(bundle: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in (bundle / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match is None:
            raise ValueError(f"malformed SHA256SUMS line: {line!r}")
        relative = safe_member(match.group(2))
        if relative in entries:
            raise ValueError(f"duplicate SHA256SUMS path: {relative}")
        entries[relative] = match.group(1)
    return entries


def validate_spdx(
    document: dict[str, object],
    *,
    version: str,
    commit: str,
    package_inputs: dict[str, tuple[Sequence[ReleaseFile], dict[str, object]]],
    schema_path: Path | None,
) -> None:
    if schema_path is not None:
        try:
            import jsonschema
        except ImportError as error:
            raise RuntimeError("--spdx-schema requires the jsonschema Python package") from error
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        jsonschema.Draft7Validator(schema).validate(document)

    assert document["spdxVersion"] == "SPDX-2.3"
    assert document["dataLicense"] == "CC0-1.0"
    assert document["SPDXID"] == "SPDXRef-DOCUMENT"
    assert document["name"] == f"game-net-core-v{version}"
    assert str(document["documentNamespace"]).endswith(f"/{commit}")
    packages = document["packages"]
    files = document["files"]
    relationships = document["relationships"]
    assert isinstance(packages, list) and len(packages) == 3
    expected_file_count = sum(len(package_files) for package_files, _ in package_inputs.values())
    assert isinstance(files, list) and len(files) == expected_file_count
    assert isinstance(relationships, list)
    package_ids = {str(package["SPDXID"]) for package in packages}
    file_ids = {str(item["SPDXID"]) for item in files}
    assert len(package_ids) == len(packages)
    assert len(file_ids) == len(files)
    assert package_ids == set(package_inputs)
    files_by_id = {str(item["SPDXID"]): item for item in files}
    for package in packages:
        assert package["licenseDeclared"] == LICENSE_ID
        assert package["licenseConcluded"] == LICENSE_ID
        assert package["filesAnalyzed"] is True
        verification_code = package["packageVerificationCode"]["packageVerificationCodeValue"]
        assert re.fullmatch(
            r"[0-9a-f]{40}",
            verification_code,
        )
        package_files, artifact = package_inputs[str(package["SPDXID"])]
        expected_sha1s = sorted(
            hashlib.sha1(item.data, usedforsecurity=False).hexdigest()
            for item in package_files
        )
        expected_code = hashlib.sha1(
            "".join(expected_sha1s).encode("ascii"), usedforsecurity=False
        ).hexdigest()
        assert verification_code == expected_code
        assert package["packageFileName"] == artifact["name"]
        assert package["checksums"] == [
            {"algorithm": "SHA256", "checksumValue": artifact["sha256"]}
        ]
    for item in files:
        assert item["licenseConcluded"] == LICENSE_ID
        assert item["licenseInfoInFiles"] == [LICENSE_ID]
        algorithms = {entry["algorithm"] for entry in item["checksums"]}
        assert algorithms == {"SHA1", "SHA256"}
    described = set(document["documentDescribes"])
    assert described == package_ids
    described_relationships = {
        relation["relatedSpdxElement"]
        for relation in relationships
        if relation["spdxElementId"] == "SPDXRef-DOCUMENT"
        and relation["relationshipType"] == "DESCRIBES"
    }
    assert described_relationships == package_ids
    contained = {
        relation["relatedSpdxElement"]
        for relation in relationships
        if relation["spdxElementId"] in package_ids
        and relation["relationshipType"] == "CONTAINS"
    }
    assert contained == file_ids
    for package_id, (package_files, _) in package_inputs.items():
        contained_ids = {
            relation["relatedSpdxElement"]
            for relation in relationships
            if relation["spdxElementId"] == package_id
            and relation["relationshipType"] == "CONTAINS"
        }
        actual_files = {
            str(files_by_id[file_id]["fileName"]).removeprefix("./"): files_by_id[file_id]
            for file_id in contained_ids
        }
        assert set(actual_files) == {item.path for item in package_files}
        for item in package_files:
            checksums = {
                checksum["algorithm"]: checksum["checksumValue"]
                for checksum in actual_files[item.path]["checksums"]
            }
            assert checksums == {
                "SHA1": hashlib.sha1(item.data, usedforsecurity=False).hexdigest(),
                "SHA256": hashlib.sha256(item.data).hexdigest(),
            }


def verify(
    bundle: Path,
    expected_commit: str,
    expected_version: str,
    spdx_schema: Path | None = None,
) -> None:
    bundle = bundle.resolve()
    required_root = {
        "LICENSE",
        "NOTICE",
        "THIRD_PARTY_NOTICES.md",
        "SHA256SUMS",
        "evidence-index.json",
        "package-build.json",
        "sbom.spdx.json",
    }
    actual_root = {path.name for path in bundle.iterdir() if path.is_file()}
    if actual_root != required_root:
        raise ValueError(f"release bundle root metadata mismatch: {actual_root ^ required_root}")

    package_build = json.loads((bundle / "package-build.json").read_text(encoding="utf-8"))
    assert package_build["schema"] == "gamenet.external_release_package.v1"
    assert package_build["external_release"] is True
    assert package_build["candidate_sha"] == expected_commit
    assert package_build["version"] == expected_version
    assert package_build["name"] == f"v{expected_version}"
    assert package_build["license"] == LICENSE_ID
    assert package_build["known_limitations"]
    artifacts = package_build["artifacts"]
    assert [item["kind"] for item in artifacts] == [
        "source-tar",
        "source-zip",
        "linux-x86_64",
        "windows-x86_64",
        "evidence",
    ]

    checksums = load_checksums(bundle)
    expected_checksum_paths = {
        "LICENSE",
        "NOTICE",
        "THIRD_PARTY_NOTICES.md",
        "evidence-index.json",
        "package-build.json",
        "sbom.spdx.json",
        *{f"artifacts/{item['name']}" for item in artifacts},
    }
    assert set(checksums) == expected_checksum_paths
    for relative, expected_hash in checksums.items():
        target = bundle / relative
        assert target.is_file(), f"missing checksummed release file: {relative}"
        assert sha256(target) == expected_hash, f"SHA256 mismatch: {relative}"
    for item in artifacts:
        target = bundle / "artifacts" / item["name"]
        assert sha256(target) == item["sha256"]
        assert target.stat().st_size == item["bytes"]
    actual_artifact_files = {
        path.name for path in (bundle / "artifacts").iterdir() if path.is_file()
    }
    expected_artifact_files = {item["name"] for item in artifacts}
    assert actual_artifact_files == expected_artifact_files

    artifact_by_kind = {item["kind"]: item for item in artifacts}
    source_tar = bundle / "artifacts" / artifact_by_kind["source-tar"]["name"]
    source_zip = bundle / "artifacts" / artifact_by_kind["source-zip"]["name"]
    tar_root, tar_files = read_tar(source_tar)
    zip_root, zip_files = read_zip(source_zip)
    assert tar_root == zip_root == f"game-net-core-v{expected_version}"
    assert file_map(tar_files) == file_map(zip_files)
    assert len(tar_files) == artifact_by_kind["source-tar"]["file_count"]
    assert len(zip_files) == artifact_by_kind["source-zip"]["file_count"]
    validate_project_metadata(tar_files, expected_version)

    spdx_package_inputs: dict[
        str, tuple[Sequence[ReleaseFile], dict[str, object]]
    ] = {
        "SPDXRef-Package-Source": (tar_files, artifact_by_kind["source-tar"])
    }
    for kind, reader, root_suffix in (
        ("linux-x86_64", read_tar, "linux-x86_64"),
        ("windows-x86_64", read_zip, "windows-x86_64"),
    ):
        artifact = artifact_by_kind[kind]
        root, files = reader(bundle / "artifacts" / artifact["name"])
        assert root == f"game-net-core-v{expected_version}-{root_suffix}"
        assert len(files) == artifact["file_count"]
        validate_project_metadata(files, expected_version)
        package_id = "SPDXRef-Package-Linux" if kind == "linux-x86_64" else "SPDXRef-Package-Windows"
        spdx_package_inputs[package_id] = (files, artifact)

    evidence = json.loads((bundle / "evidence-index.json").read_text(encoding="utf-8"))
    assert evidence["schema"] == "gamenet.release_evidence_index.v1"
    assert evidence["candidate_sha"] == expected_commit
    assert evidence["version"] == expected_version
    evidence_artifact = artifact_by_kind["evidence"]
    evidence_root, evidence_files = read_zip(bundle / "artifacts" / evidence_artifact["name"])
    assert evidence_root == f"game-net-core-v{expected_version}-evidence"
    assert len(evidence_files) == evidence["file_count"] == evidence_artifact["file_count"]
    evidence_map = file_map(evidence_files)
    assert set(evidence_map) == {item["path"] for item in evidence["evidence_files"]}
    for item in evidence["evidence_files"]:
        data = evidence_map[item["path"]]
        assert len(data) == item["bytes"]
        assert hashlib.sha256(data).hexdigest() == item["sha256"]
    promotion_record = evidence["promotion_manifest"]
    promotion_data = evidence_map[promotion_record["path"]]
    assert hashlib.sha256(promotion_data).hexdigest() == promotion_record["sha256"]
    promotion = json.loads(promotion_data)
    assert promotion["schema"] == "gamenet.production_promotion_evidence.v2"
    assert promotion["stage"] == "release"
    assert promotion["status"] == "success"
    assert promotion["candidate_sha"] == expected_commit
    assert promotion["capacity"]["profile"] == "dedicated-100k"
    assert promotion["capacity"]["endpoint_attempts"] == 100000
    durations = {item["mode"]: item["elapsed_milliseconds"] for item in promotion["endurance"]}
    assert durations["candidate-1h"] >= 3_600_000
    assert durations["release-3h"] >= 10_800_000

    source_map = file_map(tar_files)
    for name in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
        assert (bundle / name).read_bytes() == source_map[name]

    spdx = json.loads((bundle / "sbom.spdx.json").read_text(encoding="utf-8"))
    validate_spdx(
        spdx,
        version=expected_version,
        commit=expected_commit,
        package_inputs=spdx_package_inputs,
        schema_path=spdx_schema,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify a game-net-core external release bundle.")
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--expected-version", default="0.3.0")
    parser.add_argument("--spdx-schema", type=Path)
    args = parser.parse_args()
    verify(args.bundle, args.expected_commit, args.expected_version, args.spdx_schema)
    print(
        f"validated release bundle v{args.expected_version} for immutable commit "
        f"{args.expected_commit}"
    )


if __name__ == "__main__":
    main()
