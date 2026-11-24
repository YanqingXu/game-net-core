#!/usr/bin/env python3
# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import cost_ledger as c

class CostTests(unittest.TestCase):
    def test_disjoint_nearest_module_not_inclusive_caller(self):
        self.assertEqual(c.leaf_context([("gamenet::experimental::IoUringCompletionEngineImpl::enqueue", "exe"),
            ("gamenet::experimental::IoUringTcpConnectionHubImpl::send", "exe"),
            ("gamenet::experimental::IoUringEventLoopPumpImpl::driveOneTurn", "exe")]), ("user", "engine"))

    def test_kernel_not_counted_as_user_pump(self):
        self.assertEqual(c.leaf_context([("tcp_sendmsg_locked", "[kernel.kallsyms]"),
            ("IoUringEventLoopPumpImpl::driveOneTurn", "exe")]), ("kernel", "kernel_tcp_ip"))

    def test_unknown_and_allocator_remain_explicit(self):
        self.assertEqual(c.leaf_context([("[unknown]", "exe"), ("IoUringCompletionEngine::enqueue", "exe")]), ("unknown", "unknown"))
        self.assertEqual(c.leaf_context([("__memcpy_avx2", "libc"), ("IoUringCompletionEngine::enqueue", "exe")]), ("user", "allocation_copy_leaf"))
        self.assertEqual(c.leaf_context([]), ("unknown", "unknown"))

    def test_five_interleaved_blocks_with_separate_warmup(self):
        for stage, arms in (("diagnose", 2), ("compare", 4)):
            rows = list(c.plan(stage))
            self.assertEqual(len(rows), arms*16)
            self.assertEqual(len(set(rows)), len(rows))
            for case in ("saturated", "paced"):
                self.assertEqual(sum(r[0] == case for r in rows), arms*5)
            self.assertTrue(all(r[0] == "work" for r in rows if r[1] == 0))

    def test_precise_epoll_syscall_is_valid_control(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / "syscalls.txt").write_text('1.0 write(2, "GAMENET_TRACE_BEGIN\\n", 20) = 20\n'
                '1.1 epoll_pwait2(5, [], 64, NULL, NULL, 8) = 0\n'
                '1.2 write(2, "GAMENET_TRACE_END\\n", 18) = 18\n')
            self.assertEqual(c.cpu.profile_syscalls(p, "epoll")["syscalls"]["epoll_pwait2"], 1)
            with self.assertRaises(ValueError): c.cpu.profile_syscalls(p, "io_uring")

    def test_cost_lower_bound_per_echo_and_duplicate_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / "cost.csv").write_text("kind,pc,calls,bytes\n0,1234,65536,268435456\n")
            (p / "usage.csv").write_text("user_ns,100\nsystem_ns,200\n")
            c.a.write_json(p / "symbols.json", {"1234": "symbol"})
            self.assertEqual(c.cost_result(p)["per_echo"]["new"], dict(calls=1, bytes=4096))
            with (p / "cost.csv").open("a") as f: f.write("0,1234,1,2\n")
            with self.assertRaises(ValueError): c.cost_result(p)

    def test_only_engine_is_compare_runtime_delta(self):
        self.assertTrue(c.runtime(c.ENGINE))
        for name in ("include/gamenet/a.h", "src/core/a.cc", "cmake/a.cmake", "api/x.json", "CMakeLists.txt"):
            self.assertTrue(c.runtime(name))
        self.assertFalse(c.runtime("benchmarks/io_uring/cost_ledger/observer.cpp"))

    def test_perf_window_and_owner_filter(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            sample = " 10/10 1.000000100: cpu-clock:\n  1234 tcp_recvmsg ([kernel.kallsyms])\n"
            (p / "stacks.txt").write_text(sample*200 + " 20/20 1.000000100: cpu-clock:\n  1234 other (exe)\n")
            with patch.object(c.cpu, "raw_metrics", return_value=dict(owner_tid=10, measure_begin_ns=10**9, measure_end_ns=2*10**9)):
                result = c.disjoint_stacks(p)
            self.assertEqual(result["samples"], 200)
            self.assertEqual(result["contexts"], {"kernel_tcp_ip": 200})

if __name__ == "__main__": unittest.main()
