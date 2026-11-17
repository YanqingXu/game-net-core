# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Full Clang 18 TSan inventory and independent allocator detection controls."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from run_kcp_contracts import FULL_TEST_COUNT, verify_inventory, verify_results

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = "gamenet.clang_tsan_gate.v2"
FAULT_TEST = "contract.tcp_connection.test_tcp_connection_cross_thread_shutdown"
FAULT_TARGET = "gnt_contract_tcp_connection_" + hashlib.sha1(FAULT_TEST.encode()).hexdigest()[:10]
INPUTS = ["CMakeLists.txt", "cmake/Sanitizers.cmake", "tests/CMakeLists.txt",
          "tests/ci/run_clang_tsan_gate.py", "tests/ci/test_clang_tsan_gate.py",
          "tests/ci/run_kcp_contracts.py", "tests/ci/run_udp_contracts.py",
          "tests/support/ClangTsanAllocatorProbe.cpp", ".github/workflows/kcp.yml",
          "tests/contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp",
          "include/gamenet/kcp_transport/KcpTransportEndpoint.h", "src/kcp_transport/KcpTransportEndpoint.cc",
          "src/kcp_transport/CMakeLists.txt", "tests/contract/kcp/test_kcp_transport_endpoint.cpp",
          "tests/ci/kcp_examples.py", "examples/kcp_echo/EchoExample.cc", "examples/kcp_echo/EchoExample.h",
          "examples/kcp_echo/server.cpp", "examples/kcp_echo/client.cpp", "examples/CMakeLists.txt",
          "cmake/GameNetCoreConfig.cmake.in", "api/public_api_manifest.json",
          "tests/ci/run_tls_contracts.py", "tests/ci/run_websocket_contracts.py",
          "tests/ci/run_websocket_tls_contracts.py",
          "tests/cmake/kcp_install_consumer/CMakeLists.txt", "tests/cmake/kcp_install_consumer/main.cpp"]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def file_hashes(output):
    require(not output.is_symlink(), "evidence directory is a symlink")
    result = {}
    for path in output.rglob("*"):
        require(not path.is_symlink(), "evidence symlink: " + str(path))
        if path.is_file() and path != output / "manifest.json":
            result[path.relative_to(output).as_posix()] = digest(path)
    return result


def verify_probes(atomic_code, race_code, atomic_log, race_log):
    require(atomic_code == 0, "atomic control failed")
    require(not re.search(r"ThreadSanitizer|runtime error:|DEADLYSIGNAL", atomic_log),
            "atomic control reported a diagnostic")
    require(race_code == 66, "race control did not fail with exit 66")
    require(race_log.count("WARNING: ThreadSanitizer: data race") == 1,
            "race control did not report exactly one data race")
    for marker in ["SUMMARY: ThreadSanitizer: data race", "heap block of size 4",
                   "operator new(unsigned long)", "ClangTsanAllocatorProbe.cpp"]:
        require(marker in race_log, "race diagnostic missing " + marker)
    require(re.search(r"\bmalloc\b|__interceptor_malloc", race_log), "malloc interceptor stack missing")
    require(re.search(r"libclang_rt\.tsan[^\s]*\.so", race_log), "shared TSan allocation stack missing")
    require(not re.search(r"FATAL:|runtime error:|DEADLYSIGNAL|heap-use-after-free|double free", race_log),
            "race control failed for a different reason")


def verify_linkage(ninja, dynamic, symbols, runtime_dir):
    require(ninja.count("-shared-libsan") == 1, "shared runtime must be confined to the allocation contract")
    require("-fsanitize=thread" in ninja, "TSan instrumentation missing")
    require(not re.search(r"-fno-sanitize|sanitize-ignorelist|suppressions=", ninja), "sanitizer bypass")
    require(re.search(r"NEEDED.*libclang_rt\.tsan[^\s]*\.so", dynamic), "shared TSan not loaded")
    require(re.search(r"(?:RUNPATH|RPATH).*" + re.escape(runtime_dir), dynamic), "runtime search path missing")
    operators = re.findall(r"\bT operator (?:new|delete)(?:\[\])?\(", symbols)
    require(len(operators) == 6, "custom allocation replacements not retained")


def verify_output(output):
    output = Path(output)
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    require(manifest.get("schema") == SCHEMA and manifest.get("status") == "passed", "incomplete gate")
    require(manifest.get("executed_tests") == FULL_TEST_COUNT, "full inventory not executed")
    require(re.fullmatch(r"[0-9a-f]{64}", manifest.get("fault_binary_sha256", "")), "invalid fault binary identity")
    require(re.fullmatch(r"[0-9a-f]{40}", manifest.get("commit_sha", "")), "invalid source identity")
    if manifest.get("workflow_sha"):
        require(manifest["workflow_sha"] == manifest["commit_sha"] and not manifest["working_tree"],
                "CI source mismatch or dirty checkout")
    for key, variable in [("workflow_sha", "GITHUB_SHA"), ("run_id", "GITHUB_RUN_ID"),
                          ("run_attempt", "GITHUB_RUN_ATTEMPT"), ("pr_head_sha", "GAMENET_PR_HEAD_SHA")]:
        if os.getenv(variable):
            require(manifest.get(key) == os.getenv(variable), "run identity mismatch: " + key)
    require(file_hashes(output) == manifest.get("files"), "evidence inventory/hash mismatch")
    require(re.search(r"clang version 18\.", (output / "compiler.log").read_text()), "wrong compiler version")
    compiler = (output / "compiler.cmake").read_text()
    require('set(CMAKE_CXX_COMPILER_ID "Clang")' in compiler and
            re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "18\.', compiler), "configured compiler is not Clang 18")
    compile_commands = json.loads((output / "compile_commands.json").read_text())
    fault = [entry for entry in compile_commands if entry["file"].endswith(
        "tests/contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp")]
    require(len(fault) == 1 and "-fsanitize=thread" in fault[0]["command"] and
            "-std=c++23" in fault[0]["command"], "fault contract compilation is not instrumented")
    require(not re.search(r"-fno-sanitize|sanitize-ignorelist", fault[0]["command"]), "fault contract bypass")
    cache = (output / "CMakeCache.txt").read_text()
    for name, value in {"GAMENET_ENABLE_KCP": "ON", "GAMENET_ENABLE_UDP": "ON",
                        "GAMENET_ENABLE_TSAN": "ON", "GAMENET_ENABLE_ASAN_UBSAN": "OFF",
                        "GAMENET_BUILD_TESTING": "ON", "GAMENET_ENABLE_TLS": "OFF",
                        "GAMENET_ENABLE_WEBSOCKET": "OFF"}.items():
        require(f"{name}:BOOL={value}" in cache, "wrong build option: " + name)
    inventory = json.loads((output / "inventory.log").read_text())
    expected = verify_inventory(inventory)
    require(FAULT_TEST in expected, "allocation-fault contract missing")
    require(all(not any(p["name"] == "DISABLED" and p["value"] for p in test.get("properties", []))
                for test in inventory["tests"]), "disabled test")
    verify_results(ET.parse(output / "ctest-junit.xml").getroot(), expected)
    commands = manifest["commands"]
    required = {"compiler", "runtime", "configure", "inventory", "build", "ctest",
                "dynamic", "symbols", "probe-build", "probe-atomic", "probe-race"}
    require(set(commands) == required, "command inventory mismatch")
    for name, command in commands.items():
        require(command["exit_code"] == (66 if name == "probe-race" else 0), "unexpected exit: " + name)
        require(json.loads((output / (name + "-command.json")).read_text()) == command["argv"],
                "command drift: " + name)
    ctest = commands["ctest"]["argv"]
    require(not any(re.match(r"^(?:-L|-R|-E|-I|--label-regex|--exclude-regex|--exclude-label-regex|--tests-from-file|--exclude-from-file|--tests-information)", flag)
                    for flag in ctest),
            "filtered test execution")
    for flag in ["--output-junit", "--no-tests=error", "--output-on-failure"]:
        require(flag in ctest, "missing CTest gate: " + flag)
    probe_build = commands["probe-build"]["argv"]
    for flag in ["-std=c++23", "-g", "-fsanitize=thread", "-shared-libsan"]:
        require(flag in probe_build, "probe instrumentation missing: " + flag)
    require(commands["probe-atomic"]["argv"][-1] == "atomic" and
            commands["probe-race"]["argv"][-1] == "race", "wrong probe modes")
    options = manifest["environment"]["TSAN_OPTIONS"]
    require("halt_on_error=1" in options and "exitcode=66" in options and "suppressions" not in options,
            "unexpected TSan policy")
    verify_linkage((output / "build.ninja").read_text(), (output / "dynamic.log").read_text(),
                   (output / "symbols.log").read_text(), manifest["runtime_dir"])
    verify_probes(commands["probe-atomic"]["exit_code"], commands["probe-race"]["exit_code"],
                  (output / "probe-atomic.log").read_text(), (output / "probe-race.log").read_text())
    require(not re.search(r"WARNING: ThreadSanitizer|FATAL: ThreadSanitizer|runtime error:|DEADLYSIGNAL",
                          (output / "ctest.log").read_text()), "suite sanitizer diagnostic")
    for filename in INPUTS:
        require(manifest["inputs"].get(filename) == digest(output / "source" / filename), "source copy drift")
    return manifest


def run(args):
    require(sys.platform == "linux", "execution requires Linux; evidence readback is portable")
    build, output = args.build_dir.resolve(), args.output.resolve()
    require(build != ROOT and build != output and build not in output.parents and output not in build.parents,
            "build and evidence directories must be separate")
    output.mkdir(parents=True, exist_ok=True)
    require(not any(output.iterdir()), "use a fresh evidence directory")
    compiler = shutil.which(args.compiler)
    symbolizer = shutil.which("llvm-symbolizer-18")
    require(compiler and symbolizer, "Clang 18 and llvm-symbolizer-18 are required")
    env = os.environ.copy()
    env.update(TSAN_OPTIONS="halt_on_error=1:exitcode=66:external_symbolizer_path=" + symbolizer,
               DEBUGINFOD_URLS="")
    manifest = {"schema": SCHEMA, "status": "failed", "platform": platform.platform(),
                "workflow_sha": env.get("GITHUB_SHA"), "run_id": env.get("GITHUB_RUN_ID"),
                "run_attempt": env.get("GITHUB_RUN_ATTEMPT"), "pr_head_sha": env.get("GAMENET_PR_HEAD_SHA"),
                "environment": {k: env[k] for k in ["TSAN_OPTIONS", "DEBUGINFOD_URLS"]}, "commands": {}}

    def git(*arguments):
        return subprocess.check_output(["git", *arguments], cwd=ROOT, text=True).strip()

    def execute(name, command, expected=0, timeout=1800):
        command = [str(arg) for arg in command]
        print("Running " + name, flush=True)
        (output / (name + "-command.json")).write_text(json.dumps(command, indent=2) + "\n")
        started = time.monotonic()
        with (output / (name + ".log")).open("wb") as log:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        manifest["commands"][name] = {"argv": command, "exit_code": result.returncode,
                                      "elapsed_seconds": round(time.monotonic() - started, 3)}
        content = (output / (name + ".log")).read_text(errors="replace")
        if result.returncode != expected:
            print(content[-12000:], flush=True)
            raise RuntimeError(f"{name}: expected {expected}, got {result.returncode}")
        return content

    try:
        manifest.update(commit_sha=git("rev-parse", "HEAD"), working_tree=git("status", "--porcelain=v1"))
        if manifest["workflow_sha"]:
            require(manifest["workflow_sha"] == manifest["commit_sha"] and not manifest["working_tree"],
                    "exact clean CI source required")
        manifest["inputs"] = {}
        for filename in INPUTS:
            destination = output / "source" / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / filename, destination)
            manifest["inputs"][filename] = digest(destination)
        require(re.search(r"clang version 18\.", execute("compiler", [compiler, "--version"])), "Clang 18 required")
        runtime = execute("runtime", [compiler, "--print-runtime-dir"]).strip()
        require(Path(runtime).is_dir(), "compiler runtime directory missing")
        manifest["runtime_dir"] = runtime
        execute("configure", ["cmake", "-S", ROOT, "-B", build, "-G", "Ninja",
            "-DCMAKE_CXX_COMPILER=" + compiler, "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DGAMENET_BUILD_TESTING=ON", "-DGAMENET_ENABLE_UDP=ON", "-DGAMENET_ENABLE_KCP=ON",
            "-DGAMENET_ENABLE_TSAN=ON", "-DGAMENET_ENABLE_ASAN_UBSAN=OFF", "-DGAMENET_ENABLE_TLS=OFF",
            "-DGAMENET_ENABLE_WEBSOCKET=OFF", "-DGAMENET_ENABLE_EXPERIMENTAL=OFF",
            "-DGAMENET_BUILD_BENCHMARKS=OFF", "-DGAMENET_BUILD_FUZZING=OFF", "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON"])
        inventory = json.loads(execute("inventory", ["ctest", "--test-dir", build, "--show-only=json-v1"]))
        expected = verify_inventory(inventory)
        execute("build", ["cmake", "--build", build, "--parallel", args.parallel])
        execute("ctest", ["ctest", "--test-dir", build, "--output-on-failure", "--timeout", "60",
                          "--no-tests=error", "--output-junit", output / "ctest-junit.xml"])
        verify_results(ET.parse(output / "ctest-junit.xml").getroot(), expected)
        manifest["executed_tests"] = len(expected)
        shutil.copyfile(build / "CMakeCache.txt", output / "CMakeCache.txt")
        shutil.copyfile(build / "build.ninja", output / "build.ninja")
        shutil.copyfile(build / "compile_commands.json", output / "compile_commands.json")
        compiler_files = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
        require(len(compiler_files) == 1, "ambiguous compiler metadata")
        shutil.copyfile(compiler_files[0], output / "compiler.cmake")
        fault_binary = build / "tests" / FAULT_TARGET
        execute("dynamic", ["readelf", "-d", fault_binary])
        execute("symbols", ["nm", "-D", "-C", fault_binary])
        manifest["fault_binary_sha256"] = digest(fault_binary)
        probe = output / "allocation-probe"
        execute("probe-build", [compiler, "-std=c++23", "-g", "-O1", "-fsanitize=thread", "-shared-libsan",
            "-Wl,-rpath," + runtime, "-I" + str(ROOT / "tests"), ROOT / "tests/support/ClangTsanAllocatorProbe.cpp", "-o", probe])
        execute("probe-atomic", [probe, "atomic"], timeout=120)
        execute("probe-race", [probe, "race"], expected=66, timeout=120)
        verify_probes(0, 66, (output / "probe-atomic.log").read_text(), (output / "probe-race.log").read_text())
        require(git("rev-parse", "HEAD") == manifest["commit_sha"], "HEAD changed during gate")
        for filename in INPUTS:
            require(digest(ROOT / filename) == manifest["inputs"][filename], "source changed during gate: " + filename)
        manifest["status"] = "passed"
    finally:
        manifest["files"] = file_hashes(output)
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    try:
        verify_output(output)
    except Exception:
        manifest["status"] = "failed"
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        raise
    print(f"Clang 18 TSan full gate passed: {FULL_TEST_COUNT} tests, atomic exit 0, heap race exit 66", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compiler", default="clang++-18")
    parser.add_argument("--parallel", type=int, default=2, choices=range(1, 65))
    parser.add_argument("--verify-output", type=Path)
    args = parser.parse_args()
    if args.verify_output:
        verify_output(args.verify_output)
        print("Clang TSan full gate evidence readback passed")
    elif args.build_dir and args.output:
        run(args)
    else:
        parser.error("build-dir/output or verify-output required")


if __name__ == "__main__":
    main()
