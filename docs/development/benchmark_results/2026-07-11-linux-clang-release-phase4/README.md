# Local Linux Clang Release Phase 4 Baseline

This directory preserves the final-v4 local Linux half of the fixed Phase 4
benchmark scenario set, refreshed from the frozen dirty worktree.

- Snapshot directory date: 2026-07-11
- Final-v4 refresh: 2026-07-12 (Asia/Shanghai)
- Execution environment: Linux container through Docker Desktop
- Container image: `silkeh/clang:19`
- Compiler: Clang 19.1.1
- Configuration: Release
- Backend reported by the harness: epoll
- Benchmark executable build time: 2026-07-12 00:40:59 +08:00
- Raw JSON capture time: 2026-07-12 00:41:31 +08:00
- Checked-out commit before the run:
  `0d62054e148a1c95793799eb88856363ac6843d3`

The run includes uncommitted Phase 4 hardening, test, workflow, and benchmark
changes. It is dirty-worktree local development evidence, not immutable
same-SHA CI evidence, not directly comparable to the Windows host snapshot as
a platform score, and not a performance threshold. The authoritative remote
Linux/Windows pair has not been generated; a paired run on the final candidate
remains required for release closure.

Build command:

```bash
cmake -S . -B build-final-v4-linux-benchmark \
  -DCMAKE_BUILD_TYPE=Release \
  -DGAMENET_BUILD_TESTING=OFF \
  -DGAMENET_BUILD_BENCHMARKS=ON \
  -DGAMENET_ENABLE_TLS=OFF \
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-final-v4-linux-benchmark \
  --target gamenet_phase4_benchmark --parallel 2
```

The three invocation parameter sets are the fixed commands documented in
`docs/development/phase4_benchmark.md`. Raw stdout is stored unchanged in the
adjacent JSON files and is validated by `tools/validate_phase4_benchmark.py`.

Final-v4 summary:

| Scenario | Local result |
| --- | --- |
| Framing | 38,087,693.105 operations/s; 9,444.046 MiB/s |
| Logic queue | 452,934.439 operations/s; P99 37,832.501 us |
| Broadcast fanout | 5,036,891.294 operations/s; completion P99 262.500 us |
