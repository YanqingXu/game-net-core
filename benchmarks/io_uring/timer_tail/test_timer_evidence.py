#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
import tempfile
import unittest
from pathlib import Path
import timer_evidence as e

class EvidenceTests(unittest.TestCase):
    def test_partition_boundaries(self):
        spans = [(10, 20), (30, 40), (50, 60)]
        self.assertEqual(e.intersections(15, 55, spans), 20)
        self.assertEqual(e.intersections(20, 30, spans), 0)
        self.assertEqual(e.intersections(0, 100, spans), 30)

    def trace(self, lines):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / "observer.csv").write_text("wall_ns,cpu_ns,kind,value,extra\n" + lines)
        return root

    def test_partition_not_double_counted(self):
        path = self.trace("10,1,0,1000000,0\n30,3,1,0,0\n35,5,2,0,0\n45,10,3,0,0\n50,11,14,1,0\n")
        row = e.attribution(path, [(20, 50)])["worst"][0]
        self.assertEqual((row["lag_ns"], row["poll_ns"], row["message_ns"], row["remainder_ns"]), (30, 10, 10, 10))

    def test_nested_callback_outer_span_only(self):
        root = self.trace("1,1,0,0,0\n2,2,1,0,0\n3,3,2,0,0\n4,4,2,0,0\n5,5,3,0,0\n6,6,3,0,0\n")
        self.assertEqual(e.parse_observer(root / "observer.csv")[2], [(3, 6, 3)])

    def test_incomplete_rejected(self):
        for lines in ("1,1,0,0,0\n", "1,1,1,0,0\n", "1,1,0,0,0\n2,2,1,0,0\n3,3,2,0,0\n"):
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                e.parse_observer(self.trace(lines) / "observer.csv")

    def test_unknown_or_reversed_rejected(self):
        for lines in ("2,2,0,0,0\n1,3,1,0,0\n", "1,1,0,0,0\n2,2,1,0,0\n3,3,99,0,0\n"):
            with self.assertRaises(ValueError):
                e.parse_observer(self.trace(lines) / "observer.csv")

    def test_traces_and_scores_separate(self):
        rows = list(e.plan("compare"))
        self.assertEqual(len(rows), len(set(rows)))
        self.assertTrue(all(block == 0 if trace else block > 0 for _, block, _, _, trace in rows))
        self.assertEqual({label for _, _, label, _, _ in e.plan("diagnose")}, {"baseline"})

    def test_thresholds_and_quantile(self):
        s = e.stats([0, 500000, 500001, 1000000, 1000001])
        self.assertEqual((s["over_500us"], s["over_1ms"], s["p99_us"]), (3, 1, 1000.001))

if __name__ == "__main__":
    unittest.main()
