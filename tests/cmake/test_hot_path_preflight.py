# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Behavioral guard tests; observer doubles below are never lab evidence."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import hot_path_cost_common as common  # noqa: E402
import preflight_hot_path_cost as preflight  # noqa: E402
import run_hot_path_cost as runner  # noqa: E402


class ChildArtifactTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="gamenet-hp0-child-")
        self.addCleanup(temporary.cleanup)
        self.observation = Path(temporary.name) / "warmup.observation.json"

    def command(self, stdout: bytes, stderr: bytes, exit_code: int = 0) -> list[str]:
        return [
            sys.executable, "-c",
            f"import os, sys; os.write(1, {stdout!r}); os.write(2, {stderr!r}); sys.exit({exit_code})",
        ]

    def assert_retained(self, command: list[str], stdout: bytes, stderr: bytes) -> None:
        self.assertEqual(self.observation.with_suffix(".stdout.bin").read_bytes(), stdout)
        self.assertEqual(self.observation.with_suffix(".stderr.bin").read_bytes(), stderr)
        self.assertEqual(
            json.loads(self.observation.with_suffix(".command.json").read_text(encoding="utf-8")),
            {"argv": command},
        )

    def test_success_retains_original_stdout_without_json_reserialization(self) -> None:
        stdout = b'\xef\xbb\xbf  { "value" : "\xe9\x9b\xaa" }\r\n'
        stderr = b"diagnostic:\xff\r\n"
        command = self.command(stdout, stderr)
        document, returned_stderr, observation = runner.run_child(command, None, self.observation, 10, "warmup")
        self.assertEqual(document, {"value": "\u96ea"})
        self.assertEqual(returned_stderr, stderr)
        self.assertIsNone(observation)
        self.assert_retained(command, stdout, stderr)

    def test_nonzero_child_keeps_raw_stdout_stderr_before_rejection(self) -> None:
        stdout, stderr = b"partial result\x00\r\n", b"failure:\xff\n"
        command = self.command(stdout, stderr, 37)
        with self.assertRaisesRegex(common.HotPathCostError, "child failed \\(37\\)"):
            runner.run_child(command, None, self.observation, 10, "warmup")
        self.assert_retained(command, stdout, stderr)

    def test_invalid_json_and_encoding_keep_unmodified_stdout(self) -> None:
        for stdout, diagnostic in ((b"{bad json\r\n", "not one JSON document"), (b"\xff\xfe", "not strict UTF-8")):
            with self.subTest(stdout=stdout):
                command = self.command(stdout, b"")
                with self.assertRaisesRegex(common.HotPathCostError, diagnostic):
                    runner.run_child(command, None, self.observation, 10, "warmup")
                self.assert_retained(command, stdout, b"")

    def test_timeout_retains_partial_process_bytes(self) -> None:
        command = [sys.executable, "test-only-timeout"]
        timeout = subprocess.TimeoutExpired(command, 10, output=b"partial", stderr=b"diagnostic")
        with patch.object(runner.subprocess, "run", side_effect=timeout):
            with self.assertRaises(subprocess.TimeoutExpired):
                runner.run_child(command, None, self.observation, 10, "warmup")
        self.assert_retained(command, b"partial", b"diagnostic")


class HostIdentityTests(unittest.TestCase):
    def test_wsl_cannot_be_relabelled_as_native(self) -> None:
        with patch.object(common.host_platform, "system", return_value="Linux"), \
             patch.object(common.host_platform, "release", return_value="6.6.87.2-microsoft-standard-WSL2"), \
             patch.object(common.host_platform, "version", return_value="test"), \
             patch.dict(os.environ, {}, clear=True):
            self.assertTrue(common.detect_host()["wsl"])
            with self.assertRaisesRegex(common.HotPathCostError, "declared WSL state"):
                common.validate_host_identity("linux", "epoll", True, False)
            with self.assertRaisesRegex(common.HotPathCostError, "native runner"):
                common.validate_host_identity("linux", "epoll", True, True)
            self.assertTrue(common.validate_host_identity("linux", "epoll", False, True)["wsl"])

    def test_platform_declaration_and_backend_must_match_host(self) -> None:
        with patch.object(common, "detect_host", return_value={"platform": "windows", "system": "Windows", "wsl": False}):
            with self.assertRaisesRegex(common.HotPathCostError, "declared platform"):
                common.validate_host_identity("linux", "epoll", True, False)
            with self.assertRaisesRegex(common.HotPathCostError, "platform/backend"):
                common.validate_host_identity("windows", "epoll", True, False)
            self.assertEqual(common.validate_host_identity("windows", "iocp", True, False)["platform"], "windows")


class ObserverHandshakeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gamenet-observer-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.observer = self.root / "test_only_observer.py"
        self.observer.write_text(
            "import json, pathlib, subprocess, sys\n"
            "mode = sys.argv[1]\n"
            "output = pathlib.Path(sys.argv[sys.argv.index('--cost-output') + 1])\n"
            "child = subprocess.run(sys.argv[sys.argv.index('--') + 1:], capture_output=True)\n"
            "data = child.stdout\n"
            "if mode == 'corrupt-stdout': data = b'observer-prefix' + data\n"
            "sys.stdout.buffer.write(data)\n"
            "sys.stderr.buffer.write(child.stderr)\n"
            f"costs = {{key: None for key in {list(common.COST_KEYS)!r}}}\n"
            "if mode != 'missing-observation':\n"
            "    output.write_text(json.dumps({'schema': 'gamenet.hot_path_observation.v1', 'costs': costs}), encoding='utf-8')\n"
            "sys.exit(0 if mode == 'conceal-failure' else child.returncode)\n",
            encoding="utf-8",
        )

    def handshake(self, mode: str) -> list[dict]:
        return preflight.observer_handshake([sys.executable, str(self.observer), mode], self.root, 10)

    def test_executes_success_and_failure_children_with_exact_utf8_stdout(self) -> None:
        records = self.handshake("pass")
        self.assertEqual([record["observer_exit"] for record in records], [0, 37])
        self.assertNotEqual(records[0]["nonce"], records[1]["nonce"])
        for record in records:
            raw = (self.root / record["stdout_path"]).read_bytes()
            self.assertEqual(json.loads(raw)["nonce"], record["nonce"])
            self.assertEqual(record["stdout_sha256"], common.sha256_file(self.root / record["stdout_path"]))

    def test_stdout_rewriting_is_rejected(self) -> None:
        with self.assertRaisesRegex(common.HotPathCostError, "exact child stdout"):
            self.handshake("corrupt-stdout")

    def test_hidden_child_failure_is_rejected(self) -> None:
        with self.assertRaisesRegex(common.HotPathCostError, "concealed"):
            self.handshake("conceal-failure")

    def test_absent_or_stale_observation_is_rejected(self) -> None:
        with self.assertRaisesRegex(common.HotPathCostError, "cannot read observer output"):
            self.handshake("missing-observation")
        (self.root / "handshake-success.observation.json").write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(common.HotPathCostError, "must be fresh"):
            self.handshake("pass")


class ReadinessTests(unittest.TestCase):
    def test_malformed_observations_use_typed_rejection(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gamenet-hp0-malformed-") as temporary:
            path = Path(temporary) / "observation.json"
            template = {"schema": common.OBSERVATION_SCHEMA, "costs": {key: None for key in common.COST_KEYS}}
            for invalid in (True, "measured", -1, float("nan"), float("inf"), 10 ** 400):
                with self.subTest(invalid_type=type(invalid).__name__):
                    template["costs"]["cycles"] = invalid
                    path.write_text(json.dumps(template), encoding="utf-8")
                    with self.assertRaises(common.HotPathCostError):
                        common.load_observation(path)
            template["costs"]["cycles"] = None
            raw = json.dumps(template)
            for malformed in (
                raw[:-1],
                raw.replace('"cycles": null', '"cycles": null, "cycles": 0'),
                raw.replace('"cycles": null', '"cycles": ' + '9' * 5000),
            ):
                path.write_text(malformed, encoding="utf-8")
                with self.assertRaises(common.HotPathCostError):
                    common.load_observation(path)
            # Listing a capability is not a measurement of the launched child.
            template["capabilities"] = list(common.COST_KEYS)
            path.write_text(json.dumps(template), encoding="utf-8")
            observed = common.load_observation(path)
            with self.assertRaisesRegex(common.HotPathCostError, "missing required costs"):
                common.require_complete_costs(observed, {"required_costs": ["cycles"]}, "capability-only")

    def test_shortened_queued_workload_is_not_the_registered_scenario(self) -> None:
        scenario = common.load_inventory(REPO / "benchmarks/hot_path_cost_inventory.json")["scenarios"][1]
        document = {
            "schema": "gamenet.multi_io_queued_benchmark.v1", "status": "ok", "build_type": "Release",
            "clients": 4, "io_threads": 2, "messages_per_client": 5000, "payload_bytes": 256,
            "messages_per_second": 1, "mebibytes_per_second": 1,
            "network_to_logic_p99_us": 1, "network_to_logic_p999_us": 1,
            "logic_to_network_p99_us": 1, "logic_to_network_p999_us": 1,
            "queue_oldest_age_max_us": 1, "queue_depth_high_watermark": 1,
            "working_set_before_bytes": 1, "working_set_after_bytes": 1, "working_set_delta_bytes": 0,
            "producer_wake_posts": 20000, "producer_wake_merges": 0, "drain_callbacks": 1,
            "cross_domain_handoffs": 40000, "shutdown_us": 1,
        }
        common.validate_child(document, scenario, "windows", "iocp", "Release", "exact")
        document.update(messages_per_client=1, producer_wake_posts=4, cross_domain_handoffs=8)
        # Its internal accounting remains valid, but the intended 5000-message
        # workload has been shortened. Both preflight and ledger must reject it.
        with self.assertRaisesRegex(common.HotPathCostError, "preregistered arguments"):
            common.validate_child(document, scenario, "windows", "iocp", "Release", "shortened")

    def test_connection_capacity_cannot_shrink_below_registered_ten_thousand(self) -> None:
        scenario = common.load_inventory(REPO / "benchmarks/hot_path_cost_inventory.json")["scenarios"][4]
        document = {"scenario": "connections", "parameters": {"connections": 256, "event_loop_threads": 4, "settle_ms": 2000, "timeout_ms": 120000}}
        with self.assertRaisesRegex(common.HotPathCostError, "parameters.connections differs"):
            common.validate_preregistered_arguments(document, scenario, "shortened-capacity")
        document["parameters"]["connections"] = 10000
        common.validate_preregistered_arguments(document, scenario, "exact-capacity")

    def test_cost_completeness_uses_measurements_not_capability_claims(self) -> None:
        scenario = {"required_costs": ["cycles", "copied_bytes"], "cost_sources": {"copied_bytes": "copied"}}
        observed = {key: None for key in common.COST_KEYS}
        costs, _ = runner.merge_costs({"copied": 5}, scenario, observed, "probe")
        with self.assertRaisesRegex(common.HotPathCostError, "missing required costs: cycles"):
            common.require_complete_costs(costs, scenario, "probe")
        observed["cycles"] = 0  # Exact observed zero is valid; null is not.
        costs, _ = runner.merge_costs({"copied": 5}, scenario, observed, "probe")
        common.require_complete_costs(costs, scenario, "probe")
        observed["copied_bytes"] = 6
        with self.assertRaisesRegex(common.HotPathCostError, "observer/raw cost conflict"):
            runner.merge_costs({"copied": 5}, scenario, observed, "probe")

    def test_fixed_lab_aborts_on_first_incomplete_warmup(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gamenet-hp0-early-reject-") as temporary:
            root = Path(temporary)
            (root / ".git").mkdir()
            executable = root / "test-only-executable"
            executable.write_bytes(b"not performance evidence")
            args = argparse.Namespace(
                platform="windows", backend="iocp", native_runner=True, wsl=False,
                source_root=root, commit_sha="a" * 40, working_tree="clean",
                evidence_class="fixed-lab", build_type="Release", samples=10, warmups=1,
                observer_command=executable, core_executable=executable,
                phase4_executable=executable, capacity_executable=executable,
                queued_executable=executable, inventory=REPO / "benchmarks/hot_path_cost_inventory.json",
                output_root=root / "result", process_timeout_seconds=10,
            )
            with patch.object(runner, "validate_host_identity"), \
                 patch.object(runner, "git_output", side_effect=["a" * 40, ""]), \
                 patch.object(runner, "run_child", return_value=({}, b"", None)) as child, \
                 patch.object(runner, "validate_child", return_value={}):
                with self.assertRaisesRegex(common.HotPathCostError, "framing.callback-copy warmup: missing required costs"):
                    runner.run_suite(args)
            self.assertEqual(child.call_count, 1)
            self.assertFalse((root / "result/hot-path-cost.json").exists())

    def test_inspection_cannot_claim_ready_or_launch_probes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gamenet-hp0-inspect-") as temporary:
            root = Path(temporary)
            args = argparse.Namespace(
                platform="windows", backend="iocp", source_root=root, commit_sha=None,
                runner_id="", frequency_policy="", observer_command=None,
                core_executable=None, phase4_executable=None, capacity_executable=None,
                queued_executable=None, inventory=REPO / "benchmarks/hot_path_cost_inventory.json",
                output_root=root, probe=False, process_timeout_seconds=10,
            )
            with patch.object(preflight, "validate_host_identity"), \
                 patch.object(preflight, "git_output", side_effect=["a" * 40, ""]), \
                 patch.object(preflight, "observer_handshake") as handshake:
                result = preflight.run_preflight(args)
            handshake.assert_not_called()
            self.assertEqual(result["status"], "blocked")
            self.assertFalse(result["promotion_eligible"])
            self.assertIn("allocations", result["observer_required_costs"]["framing.callback-copy"])
            self.assertTrue(any("observer is missing" in blocker for blocker in result["blockers"]))

    def test_probe_flag_cannot_bypass_wsl_rejection(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gamenet-hp0-host-reject-") as temporary:
            root = Path(temporary)
            executable = root / "test-only-executable"
            executable.write_bytes(b"never executed")
            args = argparse.Namespace(
                platform="linux", backend="epoll", source_root=root, commit_sha=None,
                runner_id="unit-test", frequency_policy="unit-test", observer_command=executable,
                core_executable=executable, phase4_executable=executable,
                capacity_executable=executable, queued_executable=executable,
                inventory=REPO / "benchmarks/hot_path_cost_inventory.json",
                output_root=root, probe=True, process_timeout_seconds=10,
            )
            with patch.object(common, "detect_host", return_value={"platform": "linux", "system": "Linux", "wsl": True}), \
                 patch.object(preflight, "git_output", side_effect=["a" * 40, ""]), \
                 patch.object(preflight, "observer_handshake") as handshake:
                result = preflight.run_preflight(args)
            handshake.assert_not_called()
            self.assertEqual(result["status"], "blocked")
            self.assertTrue(any("WSL state" in blocker for blocker in result["blockers"]))


if __name__ == "__main__":
    unittest.main()
