#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

"""Portable unit checks for the evidence boundary; not native performance evidence."""

import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

import cpu_attribution as profiling
import cpu_variant as variant
import dispatch_budget as dispatch
import run_acceptance as acceptance

SHA = "a" * 40
OTHER = "b" * 40


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def probe(self, kernel="6.8.0-azure"):
        return dict(system="Linux", kernel=kernel, proc_osrelease=kernel,
                    proc_version=f"Linux version {kernel} (Microsoft build compiler)",
                    wsl_runtime_paths=[], wsl_environment=[])

    def test_native_kernel_with_vendor_build_label(self):
        acceptance.check_native_kernel(self.probe())

    def test_wsl_kernel_and_runtime_are_rejected(self):
        probes = [self.probe("6.6.87.2-microsoft-standard-WSL2"),
                  {**self.probe(), "wsl_environment": ["WSL_INTEROP"]},
                  {**self.probe(), "wsl_runtime_paths": ["/run/WSL"]}]
        for probe in probes:
            with self.subTest(probe=probe), self.assertRaises(ValueError):
                acceptance.check_native_kernel(probe)

    def test_disagreeing_kernel_sources_are_rejected(self):
        with self.assertRaises(ValueError):
            acceptance.check_native_kernel({**self.probe(), "proc_osrelease": "different"})

    def test_replay_revision_identity(self):
        evidence = dict(core_revision=SHA, consumer_revision=SHA)
        acceptance.check_revisions(evidence, SHA, SHA)
        for source, consumer in ((OTHER, SHA), (SHA, OTHER)):
            with self.subTest(source=source, consumer=consumer), self.assertRaises(ValueError):
                acceptance.check_revisions(evidence, source, consumer)

    def test_malformed_revision_rejected(self):
        for value in (None, "a" * 39, "A" * 40):
            with self.subTest(value=value), self.assertRaises(ValueError):
                acceptance.check_revisions(dict(core_revision=value, consumer_revision=SHA))

    def test_manifest_tracks_content_and_inventory(self):
        file = self.root / "raw.csv"
        file.write_bytes(b"a,1\n")
        first = acceptance.file_manifest(self.root)
        (self.root / "manifest.json").write_text("{}", encoding="utf-8")
        self.assertEqual(first, acceptance.file_manifest(self.root))
        file.write_bytes(b"a,2\n")
        self.assertNotEqual(first, acceptance.file_manifest(self.root))
        self.assertEqual(first["raw.csv"]["bytes"], 4)

    def test_duplicate_frame_is_not_counted_twice(self):
        file = self.root / "accepted.csv"
        file.write_text("kind,peer,sequence,accepted_ns\nE,0,0,1\nE,0,0,2\n", encoding="ascii")
        with self.assertRaises(ValueError):
            acceptance.unique_rows(file, "kind,peer,sequence,accepted_ns", acceptance.frame_key)

    def contracts(self, names, failure=None):
        report = ET.Element("testsuite")
        for name in names:
            case = ET.SubElement(report, "testcase", name=name)
            if failure:
                ET.SubElement(case, failure)
        directory = self.root / "variant/build"
        directory.mkdir(parents=True, exist_ok=True)
        ET.ElementTree(report).write(directory / "io-uring-contracts.xml")

    def test_exact_eleven_contracts(self):
        self.contracts(variant.CONTRACTS)
        self.assertEqual(len(variant.validate_contracts(self.root)), 11)

    def test_contract_identity_and_duplicate_rejected(self):
        names = sorted(variant.CONTRACTS)
        for changed in (names[:-1], names + names[:1],
                        names[:-1] + ["contract.io_engine.test_unrelated"]):
            self.contracts(changed)
            with self.assertRaises(ValueError):
                variant.validate_contracts(self.root)

    def test_failed_or_skipped_contract_rejected(self):
        for tag in ("failure", "error", "skipped"):
            self.contracts(variant.CONTRACTS, tag)
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                variant.validate_contracts(self.root)

    def test_fixed_trial_plans(self):
        self.assertEqual(len(list(acceptance.plan())), 24)
        self.assertEqual(len(list(variant.trial_plan())), 28)
        self.assertEqual(len(list(dispatch.trial_plan())), 48)
        self.assertEqual(set(dispatch.ORDERS), set(__import__("itertools").permutations((16, 32, 64))))

    def test_timer_cadence_and_cancellation(self):
        raw = dict(timer_period_ns=1_000_000, timer_started_ns=0,
                   timer_first_deadline_ns=1_000_000, measure_begin_ns=500_000,
                   measure_end_ns=3_000_000, timer_cancelled_ns=3_100_000,
                   timer_cancelled_deadline_ns=3_000_000, stop_requested_ns=5_000_000)
        (self.root / "timer.csv").write_text(
            "deadline_ns,callback_ns\n1000000,1010000\n2000000,2020000\n", encoding="ascii")
        result = dispatch.read_timer(self.root, raw)
        self.assertEqual((result["callbacks"], result["missed_ticks"]), (2, 0))
        with self.assertRaises(ValueError):
            dispatch.read_timer(self.root, {**raw, "timer_cancelled_deadline_ns": 4_000_000})

    def test_syscall_window_and_submission_accounting(self):
        (self.root / "syscalls.txt").write_text(
            '1.0 io_uring_enter(3, 5, 0, 0, NULL, 8) = 5\n'
            '1.1 write(2, "GAMENET_TRACE_BEGIN\\n", 20) = 20\n'
            '1.2 io_uring_enter(3, 2, 0, 0, NULL, 8) = 2\n'
            '1.3 write(2, "GAMENET_TRACE_END\\n", 18) = 18\n', encoding="ascii")
        result = profiling.profile_syscalls(self.root, "io_uring")
        self.assertEqual(result["sqes_submitted"], 2)
        self.assertEqual(result["syscalls"], {"io_uring_enter": 1})

    def test_missing_trace_boundary_rejected(self):
        (self.root / "syscalls.txt").write_text('1.0 io_uring_enter(3, 2, 0, 0, NULL, 8) = 2\n')
        with self.assertRaises(ValueError):
            profiling.profile_syscalls(self.root, "io_uring")

    def fresh_fixture(self):
        inputs = self.root / "inputs"
        inputs.mkdir()
        files = {}
        for name in (*acceptance.ARCHIVES, "installed_io_uring_acceptance"):
            (inputs / name).write_bytes(name.encode())
            files[f"variant/inputs/{name}"] = dispatch.fingerprint(inputs / name)
        for name, member in (("package-source.diff", "variant/source.diff"),
                             (dispatch.PATCH, f"variant/{dispatch.PATCH}")):
            (self.root / name).write_bytes(b"bounded experiment delta\n")
            files[member] = dispatch.fingerprint(self.root / name)
        manifest = self.root / "predecessor-manifest.json"
        acceptance.write_json(manifest, dict(schema=variant.SCHEMA, core_revision=SHA,
                                            consumer_revision=SHA, files=files))
        return dict(core_revision=SHA, consumer_revision=SHA,
                    manifest_sha256=dispatch.fingerprint(manifest)["sha256"])

    def test_fresh_install_binds_both_archives_and_consumer(self):
        predecessor = self.fresh_fixture()
        with patch.object(acceptance, "check_build"):
            dispatch.check_install(self.root, "/prefix", predecessor)
            (self.root / "inputs/installed_io_uring_acceptance").write_bytes(b"different consumer")
            with self.assertRaises(ValueError):
                dispatch.check_install(self.root, "/prefix", predecessor)

    def test_fresh_predecessor_digest_and_identity(self):
        predecessor = self.fresh_fixture()
        with patch.object(acceptance, "check_build"):
            for altered in ({**predecessor, "manifest_sha256": "0" * 64},
                            {**predecessor, "core_revision": OTHER}):
                with self.subTest(altered=altered), self.assertRaises(ValueError):
                    dispatch.check_install(self.root, "/prefix", altered)

    def test_historical_predecessor_not_replaced_by_fresh_mode(self):
        self.fresh_fixture()
        with self.assertRaises(ValueError):
            dispatch.predecessor_manifest(self.root / "predecessor-manifest.json")

    def test_workflows_are_rerunnable_and_pinned(self):
        workflows = Path(__file__).resolve().parents[3] / ".github/workflows"
        for name in ("io-uring-installed-acceptance.yml", "io-uring-cpu-attribution.yml"):
            text = (workflows / name).read_text(encoding="utf-8")
            self.assertIn("PACKAGE_REVISION: ${{ github.sha }}", text)
            self.assertIn("workflow_dispatch:", text)
            self.assertNotIn("branches: [codex/", text)
        text = (workflows / "io-uring-dispatch-budget.yml").read_text(encoding="utf-8")
        self.assertNotIn("download-artifact", text)
        self.assertIn("dispatch_budget: true", text)


if __name__ == "__main__":
    unittest.main()
