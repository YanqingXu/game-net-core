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
import hot_path_compare as hot_path
import submission_compare as submission
import run_acceptance as acceptance

SHA = "a" * 40
OTHER = "b" * 40


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_hot_path_requires_fixed_baseline_and_exact_candidate(self):
        evidence = dict(baseline_revision=hot_path.BASELINE, candidate_revision=SHA, consumer_revision=SHA)
        hot_path.check_identity(evidence, SHA)
        for field in evidence:
            with self.subTest(field=field), self.assertRaises(ValueError):
                hot_path.check_identity({**evidence, field: OTHER}, SHA)

    def test_submission_fixed_baseline_and_scope(self):
        evidence = dict(baseline_revision=submission.BASELINE, candidate_revision=SHA, consumer_revision=SHA)
        submission.check_identity(evidence, SHA)
        for field in evidence:
            with self.subTest(field=field), self.assertRaises(ValueError):
                submission.check_identity({**evidence, field: OTHER}, SHA)
        submission.check_source_delta(submission.CHANGED)
        for path in ("src/core/net/EventLoop.cc", "include/gamenet/experimental/io_uring/IoUringEventLoopPump.h",
                     "benchmarks/io_uring/installed_acceptance/acceptance.cpp", "CMakeLists.txt"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                submission.check_source_delta(submission.CHANGED | {path})

    def test_submission_balanced_factorial_and_timer_control(self):
        rows = list(submission.plan())
        self.assertEqual(len(rows), 158)
        for case, count, arms in (("steady", 15, set(submission.ARMS)),
                                  ("timer", 21, {"epoll", "before", "after"})):
            firsts = []
            for pair in range(1, count + 1):
                found = [arm for kind, block, arm in rows if kind == case and block == pair]
                self.assertEqual(set(found), arms)
                self.assertEqual(len(found), len(arms))
                firsts.append(found[0])
            self.assertTrue(all(firsts.count(arm) == count // len(arms) for arm in arms))
        self.assertEqual(submission.PROFILE["notice_budget"], 16)
        self.assertEqual(submission.PROFILE["timer_period_ns"], 1_000_000)

    def test_submission_single_factor_sources_cannot_be_mixed(self):
        for label in submission.LABELS:
            for path in submission.CHANGED:
                source = "new" if label == "candidate" or (label == "batch" and path == submission.PUMP) \
                    or (label == "copy" and path == submission.ENGINE) else "old"
                file = self.root / label / "source" / path
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(source + path, encoding="ascii")
        submission.check_derivation(self.root)
        (self.root / "batch/source" / submission.ENGINE).write_text("uncontrolled change")
        with self.assertRaises(ValueError):
            submission.check_derivation(self.root)

    def test_submission_requires_all_fifteen_pairs(self):
        with self.assertRaises(ValueError):
            submission.paired_interval([1.0] * 14)
        self.assertEqual(submission.paired_interval([1.0] * 15)["bootstrap_95_percent"], [1.0, 1.0])

    def test_submission_preserves_historical_verifiers(self):
        self.assertNotEqual(submission.BASELINE, hot_path.BASELINE)
        self.assertEqual(hot_path.PROFILE["timer_pairs"], 3)
        self.assertEqual(hot_path.CHANGED, {submission.ENGINE, "src/experimental/io_uring/IoUringTcpConnectionHub.cc"})

    def test_hot_path_runtime_delta_is_narrow_and_consumer_unchanged(self):
        changed = hot_path.CHANGED | {"tests/contract/io_engine/test_io_uring_completion_engine.cpp"}
        hot_path.check_source_delta(changed)
        for path in ("src/core/net/TcpConnection.cc", "include/gamenet/experimental/io_uring/IoUringCompletionEngine.h",
                     "CMakeLists.txt", "cmake/Sanitizers.cmake", "api/experimental_io_uring.json",
                     "benchmarks/io_uring/installed_acceptance/acceptance.cpp",
                     "benchmarks/io_uring/installed_acceptance/CMakeLists.txt"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                hot_path.check_source_delta(changed | {path})
        with self.assertRaises(ValueError):
            hot_path.check_source_delta(set())

    def test_hot_path_balanced_pairs_and_fixed_limits(self):
        rows = list(hot_path.plan())
        self.assertEqual(len(rows), 54)  # 39 steady (including warmup), 9 slow, 6 timer.
        for pair in range(hot_path.PAIRS + 1):
            self.assertEqual({arm for case, block, arm in rows if case == "steady" and block == pair},
                             set(hot_path.ARMS))
        firsts = [next(arm for case, block, arm in rows if case == "steady" and block == pair)
                  for pair in range(1, hot_path.PAIRS + 1)]
        self.assertTrue(all(firsts.count(arm) == 4 for arm in hot_path.ARMS))
        self.assertEqual(hot_path.PROFILE["notice_budget"], 16)
        self.assertEqual(hot_path.PROFILE["output_high_water_bytes"], 65536)

    def test_hot_path_paired_interval_rejects_missing_pair(self):
        with self.assertRaises(ValueError):
            hot_path.paired_interval([5.0] * 11)
        result = hot_path.paired_interval([5.0] * 12)
        self.assertEqual(result["paired_median_percent"], 5.0)
        self.assertEqual(result["bootstrap_95_percent"], [5.0, 5.0])

    def test_hot_path_contracts_reject_skips_and_duplicates(self):
        self.contracts(variant.CONTRACTS)
        path = self.root / "variant/build/io-uring-contracts.xml"
        self.assertEqual(len(hot_path.contracts(path)), 11)
        self.contracts(variant.CONTRACTS, failure="skipped")
        with self.assertRaises(ValueError):
            hot_path.contracts(path)
        self.contracts([*variant.CONTRACTS, next(iter(variant.CONTRACTS))])
        with self.assertRaises(ValueError):
            hot_path.contracts(path)

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
