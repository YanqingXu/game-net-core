# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Build real optional TLS, verify exact execution and installed dependency isolation."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

CONTRACTS = {"unit.tls.test_tls_context", "contract.tls.test_tls_stream", "integration.tls.test_tls_tcp",
             "contract.rpc_tls.test_rpc_tls_session", "integration.rpc_tls.test_rpc_tls_tcp"}

def verify_inventory(document: dict) -> set[str]:
    tests = document.get("tests", [])
    names = [t["name"] for t in tests]
    if len(names) != 138 or len(set(names)) != 138:
        raise ValueError("TLS-enabled inventory must contain exactly 138 unique tests")
    selected = set()
    for test in tests:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        if "tls" in props.get("LABELS", []):
            if props.get("DISABLED", False): raise ValueError("TLS contract is disabled")
            selected.add(test["name"])
    if selected != CONTRACTS: raise ValueError("TLS selection must be exactly five real-OpenSSL tests")
    return set(names)

def verify_results(root: ET.Element, expected: set[str]) -> None:
    cases = root.findall(".//testcase")
    if len(cases) != len(expected) or {c.get("name") for c in cases} != expected:
        raise ValueError("TLS execution does not match the expected inventory")
    for case in cases:
        if case.get("status") != "run" or any(case.find(k) is not None for k in ("failure", "error", "skipped")):
            raise ValueError("TLS evidence contains failed, skipped or unexecuted tests")

def run(args: argparse.Namespace) -> None:
    repo = Path(__file__).resolve().parents[2]
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if args.openssl_root:
        env["OPENSSL_ROOT_DIR"] = str(args.openssl_root.resolve())
        env["PATH"] = str(args.openssl_root.resolve() / "bin") + os.pathsep + env.get("PATH", "")
    if args.profile == "asan":
        env["ASAN_OPTIONS"] = "halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    if args.profile == "tsan": env["TSAN_OPTIONS"] = "halt_on_error=1"
    configuration = "Release" if args.profile == "release" else "Debug"
    manifest = {"schema": "gamenet.tls_contracts.v1", "profile": args.profile, "status": "failed",
                "workflow_sha": env.get("GITHUB_SHA"), "run_id": env.get("GITHUB_RUN_ID"),
                "run_attempt": env.get("GITHUB_RUN_ATTEMPT"), "contracts": sorted(CONTRACTS)}
    def execute(name: str, command: list[str], *, succeeds: bool = True) -> str:
        (output / (name + "-command.json")).write_text(json.dumps(command, indent=2) + "\n", encoding="utf-8")
        print("Running " + name, flush=True)
        with (output / (name + ".log")).open("wb") as log:
            result = subprocess.run(command, cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=1800)
        text = (output / (name + ".log")).read_text(encoding="utf-8", errors="replace")
        if (result.returncode == 0) != succeeds:
            print(text[-16000:], flush=True); raise RuntimeError(f"{name}: unexpected exit {result.returncode}")
        return text
    def configure(directory: Path, tls: bool) -> list[str]:
        command = ["cmake", "-S", str(repo), "-B", str(directory), "-DCMAKE_BUILD_TYPE=" + configuration,
                   "-DGAMENET_ENABLE_TLS=" + ("ON" if tls else "OFF"), "-DGAMENET_BUILD_TESTING=ON",
                   "-DGAMENET_ENABLE_EXPERIMENTAL=OFF", "-DGAMENET_BUILD_BENCHMARKS=OFF", "-DGAMENET_BUILD_FUZZING=OFF"]
        command += ["-A", "x64"] if os.name == "nt" else ["-G", "Ninja"]
        if tls and args.profile == "asan": command.append("-DGAMENET_ENABLE_ASAN_UBSAN=ON")
        if tls and args.profile == "tsan": command.append("-DGAMENET_ENABLE_TSAN=ON")
        if not tls: command.append("-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON")
        if args.openssl_root: command.append("-DOPENSSL_ROOT_DIR=" + str(args.openssl_root.resolve()))
        return command
    def consumer(name: str, prefix: Path, component: str | None, succeeds: bool = True) -> None:
        directory = build / name
        command = ["cmake", "-S", str(repo / "tests/cmake/tls_install_consumer"), "-B", str(directory),
                   "-DCMAKE_BUILD_TYPE=" + configuration, "-DCMAKE_PREFIX_PATH=" + str(prefix),
                   "-DEXPECT_TLS=" + ("ON" if component == "tls" else "OFF"),
                   "-DEXPECT_RPC_TLS=" + ("ON" if component == "rpc_tls" else "OFF"),
                   "-DEXPECT_RPC=" + ("ON" if component == "rpc" else "OFF")]
        command += ["-A", "x64"] if os.name == "nt" else ["-G", "Ninja"]
        if args.openssl_root: command.append("-DOPENSSL_ROOT_DIR=" + str(args.openssl_root.resolve()))
        if component not in {"tls", "rpc_tls"}: command.append("-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON")
        text = execute(name + "-configure", command, succeeds=succeeds)
        if not succeeds:
            if f"Required component {component} is unavailable" not in text: raise ValueError("OFF package failed for the wrong reason")
            return
        execute(name + "-build", ["cmake", "--build", str(directory), "--config", configuration, "--parallel", "2"])
        execute(name + "-test", ["ctest", "--test-dir", str(directory), "-C", configuration, "--output-on-failure", "--no-tests=error"])
    try:
        manifest["commit_sha"] = execute("commit", ["git", "rev-parse", "HEAD"]).strip()
        manifest["working_tree"] = execute("worktree", ["git", "status", "--porcelain=v1"])
        if manifest["workflow_sha"] and (manifest["workflow_sha"] != manifest["commit_sha"] or manifest["working_tree"]):
            raise ValueError("TLS CI requires the exact clean workflow commit")
        execute("configure", configure(build, True))
        # The compiler directory also supplies MSVC's dynamic ASan runtime for
        # installed consumers (which do not inherit source-tree CTest settings).
        for path in (build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"):
            match = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)', path.read_text(encoding="utf-8"))
            if match: env["PATH"] = str(Path(match[1]).parent) + os.pathsep + env["PATH"]; break
        inventory_text = execute("inventory", ["ctest", "--test-dir", str(build), "-C", configuration, "--show-only=json-v1"])
        names = verify_inventory(json.loads(inventory_text))
        targets = ["gnt_" + n.split(".")[0] + "_" + n.split(".")[1] + "_" + hashlib.sha1(n.encode()).hexdigest()[:10] for n in sorted(CONTRACTS)]
        api = json.loads((repo / "api/public_api_manifest.json").read_text(encoding="utf-8"))
        installed = ["gamenet_" + name.split("::")[1] for group in api["targets"].values() for name in group]
        command = ["cmake", "--build", str(build), "--config", configuration, "--parallel", str(args.jobs)]
        if args.profile != "release": command += ["--target", *installed, *targets]
        execute("build", command)
        junit = output / "ctest-junit.xml"
        command = ["ctest", "--test-dir", str(build), "-C", configuration, "--output-on-failure", "--timeout", "60",
                   "--no-tests=error", "--output-junit", str(junit)]
        if args.profile != "release": command += ["-L", "^tls$"]
        execute("ctest", command)
        expected = names if args.profile == "release" else CONTRACTS
        verify_results(ET.parse(junit).getroot(), expected)
        manifest["executed_tests"] = len(expected)
        prefix = build / "prefix"
        execute("install", ["cmake", "--install", str(build), "--config", configuration, "--prefix", str(prefix)])
        consumer("tls-consumer", prefix, "tls"); consumer("rpc-tls-consumer", prefix, "rpc_tls")
        consumer("core-only-consumer", prefix, None); consumer("rpc-only-consumer", prefix, "rpc")
        off = build / "off"
        execute("off-configure", configure(off, False))
        if args.profile == "release":
            execute("off-build", ["cmake", "--build", str(off), "--config", configuration, "--parallel", str(args.jobs),
                                  "--target", *[x for x in installed if x not in {"gamenet_tls", "gamenet_rpc_tls"}]])
            off_prefix = off / "prefix"
            execute("off-install", ["cmake", "--install", str(off), "--config", configuration, "--prefix", str(off_prefix)])
            if any((off_prefix / path).exists() for path in ("include/gamenet/tls", "include/gamenet/rpc_tls")) or any(
                    list(off_prefix.rglob(name)) for name in ("GameNetTlsTargets.cmake", "GameNetRpcTlsTargets.cmake")):
                raise ValueError("OFF package leaked TLS headers/export")
            consumer("off-core-consumer", off_prefix, None); consumer("off-rpc-consumer", off_prefix, "rpc")
            consumer("off-tls-rejection", off_prefix, "tls", False)
            consumer("off-rpc-tls-rejection", off_prefix, "rpc_tls", False)
            manifest["off_installed_isolation"] = "passed"
        manifest["status"] = "passed"
    finally:
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("release", "asan", "tsan"), required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--openssl-root", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    try: run(args)
    except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired, ET.ParseError) as error:
        print("TLS verification failed: " + str(error), file=sys.stderr); return 1
    return 0
if __name__ == "__main__": raise SystemExit(main())
