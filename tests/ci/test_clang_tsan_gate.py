# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Reject false-green full-suite and allocator-control evidence."""
import json
import os
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

from run_clang_tsan_gate import FAULT_TEST, INPUTS, SCHEMA, digest, file_hashes, verify_output
from run_kcp_contracts import CONTRACTS, UDP_CONTRACTS, FULL_TEST_COUNT


class GateEvidence(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict(os.environ, {}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.output = Path(directory.name)
        self.manifest = {"schema": SCHEMA, "status": "passed", "executed_tests": FULL_TEST_COUNT,
            "fault_binary_sha256": "f" * 64,
            "commit_sha": "a" * 40, "working_tree": "", "workflow_sha": None,
            "runtime_dir": "/compiler/runtime", "environment": {"TSAN_OPTIONS": "halt_on_error=1:exitcode=66"},
            "commands": {}, "inputs": {}}
        for name in ["compiler", "runtime", "configure", "inventory", "build", "ctest", "dynamic",
                     "symbols", "probe-build", "probe-atomic", "probe-race"]:
            argv = [name]
            if name == "ctest":
                argv += ["--output-junit", "results.xml", "--no-tests=error", "--output-on-failure"]
            if name == "probe-build":
                argv += ["-std=c++23", "-g", "-fsanitize=thread", "-shared-libsan"]
            if name.startswith("probe-") and name != "probe-build":
                argv += ["race" if name == "probe-race" else "atomic"]
            self.manifest["commands"][name] = {"argv": argv, "exit_code": 66 if name == "probe-race" else 0}
            self.write(name + "-command.json", json.dumps(argv))
            self.write(name + ".log", "")
        names = [FAULT_TEST, *[f"baseline.test.{i}" for i in range(132)], *sorted(CONTRACTS | UDP_CONTRACTS)]
        tests = [{"name": name, "properties": [{"name": "LABELS", "value": ["kcp"] if name in CONTRACTS else []}]} for name in names]
        self.write("inventory.log", json.dumps({"tests": tests}))
        suite = ET.Element("testsuite")
        for name in names:
            ET.SubElement(suite, "testcase", {"name": name, "status": "run"})
        ET.ElementTree(suite).write(self.output / "ctest-junit.xml")
        self.write("compiler.log", "Ubuntu clang version 18.1.3")
        self.write("compiler.cmake", 'set(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER_VERSION "18.1.3")')
        self.write("CMakeCache.txt", "\n".join(f"{k}:BOOL={v}" for k, v in {
            "GAMENET_ENABLE_KCP": "ON", "GAMENET_ENABLE_UDP": "ON", "GAMENET_ENABLE_TSAN": "ON",
            "GAMENET_ENABLE_ASAN_UBSAN": "OFF", "GAMENET_BUILD_TESTING": "ON",
            "GAMENET_ENABLE_TLS": "OFF", "GAMENET_ENABLE_WEBSOCKET": "OFF"}.items()))
        self.write("compile_commands.json", json.dumps([{"file": "tests/contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp",
            "command": "clang++-18 -fsanitize=thread -std=c++23"}]))
        self.write("build.ninja", "-fsanitize=thread -shared-libsan")
        self.write("dynamic.log", "NEEDED libclang_rt.tsan-x86_64.so\nRUNPATH /compiler/runtime")
        self.write("symbols.log", "\n".join("0000 T operator " + name for name in [
            "new(unsigned long)", "new[](unsigned long)", "delete(void*)", "delete[](void*)",
            "delete(void*, unsigned long)", "delete[](void*, unsigned long)"]))
        self.write("probe-race.log", "WARNING: ThreadSanitizer: data race\nLocation is heap block of size 4\n"
            "malloc libclang_rt.tsan-x86_64.so\noperator new(unsigned long)\nClangTsanAllocatorProbe.cpp\n"
            "SUMMARY: ThreadSanitizer: data race\n")
        for name in INPUTS:
            self.write("source/" + name, "retained source: " + name)
            self.manifest["inputs"][name] = digest(self.output / "source" / name)
        self.resign()

    def write(self, name, content):
        path = self.output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def resign(self):
        self.manifest["files"] = file_hashes(self.output)
        self.write("manifest.json", json.dumps(self.manifest))

    def rejected(self):
        with self.assertRaises(ValueError):
            verify_output(self.output)

    def test_valid_evidence(self):
        verify_output(self.output)

    def test_hash_tampering(self):
        self.write("probe-race.log", "forged result")
        self.rejected()

    def test_probe_success_cannot_replace_race(self):
        self.manifest["commands"]["probe-race"]["exit_code"] = 0
        self.resign()
        self.rejected()

    def test_wrong_diagnostic_even_with_exit_66(self):
        self.write("probe-race.log", "FATAL: ThreadSanitizer: unexpected memory mapping")
        self.resign()
        self.rejected()

    def test_atomic_control_must_be_clean(self):
        self.write("probe-atomic.log", "WARNING: ThreadSanitizer: data race")
        self.resign()
        self.rejected()

    def test_skipped_test_cannot_count_as_executed(self):
        tree = ET.parse(self.output / "ctest-junit.xml")
        ET.SubElement(tree.getroot()[0], "skipped")
        tree.write(self.output / "ctest-junit.xml")
        self.resign()
        self.rejected()

    def test_disabled_registered_test(self):
        path = self.output / "inventory.log"
        data = json.loads(path.read_text())
        data["tests"][0]["properties"].append({"name": "DISABLED", "value": True})
        self.write("inventory.log", json.dumps(data))
        self.resign()
        self.rejected()

    def test_filtered_ctest(self):
        self.manifest["commands"]["ctest"]["argv"] += ["-L", "threading"]
        self.write("ctest-command.json", json.dumps(self.manifest["commands"]["ctest"]["argv"]))
        self.resign()
        self.rejected()

    def test_shared_runtime_cannot_spread_to_other_targets(self):
        self.write("build.ninja", "-fsanitize=thread -shared-libsan -shared-libsan")
        self.resign()
        self.rejected()

    def test_long_form_ctest_filter(self):
        self.manifest["commands"]["ctest"]["argv"] += ["--label-regex=threading"]
        self.write("ctest-command.json", json.dumps(self.manifest["commands"]["ctest"]["argv"]))
        self.resign()
        self.rejected()

    def test_gcc_cannot_impersonate_clang(self):
        self.write("compiler.cmake", 'set(CMAKE_CXX_COMPILER_ID "GNU")')
        self.resign()
        self.rejected()

    def test_uninstrumented_fault_contract(self):
        self.write("compile_commands.json", json.dumps([{"file": "tests/contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp", "command": "clang++-18 -std=c++23"}]))
        self.resign()
        self.rejected()

    def test_changed_source_copy(self):
        self.write("source/" + INPUTS[0], "different source")
        self.resign()
        self.rejected()

    def test_explicit_instrumentation_disable(self):
        self.write("compile_commands.json", json.dumps([{"file": "tests/contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp",
            "command": "clang++-18 -std=c++23 -fsanitize=thread -fno-sanitize=thread"}]))
        self.resign()
        self.rejected()

    def test_invalid_binary_identity(self):
        self.manifest["fault_binary_sha256"] = "missing"
        self.resign()
        self.rejected()

    def test_wrong_ci_identity(self):
        with patch.dict(os.environ, {"GITHUB_SHA": "b" * 40}):
            self.rejected()

    def test_suppression_policy(self):
        self.manifest["environment"]["TSAN_OPTIONS"] += ":suppressions=ignore.txt"
        self.resign()
        self.rejected()


if __name__ == "__main__":
    unittest.main()
