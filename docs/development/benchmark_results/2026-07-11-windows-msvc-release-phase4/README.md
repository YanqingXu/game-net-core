# Local Windows MSVC Release Phase 4 Baseline

This directory preserves the final-v4 local execution of the fixed Phase 4
benchmark scenario set, refreshed from the frozen dirty worktree.

- Snapshot directory date: 2026-07-11
- Final-v4 refresh: 2026-07-12 (Asia/Shanghai)
- Host OS: Windows 10 Pro 10.0.19045
- Generator: Visual Studio 17 2022, x64
- Compiler: MSVC 19.44.35214
- Configuration: Release
- Backend reported by the harness: IOCP
- Benchmark executable build time: 2026-07-12 00:26:37 +08:00
- Raw JSON refresh time: 2026-07-12 00:27:22 +08:00
- Checked-out commit before the run:
  `0d62054e148a1c95793799eb88856363ac6843d3`

The run includes uncommitted Phase 4 hardening and benchmark changes. It is
dirty-worktree local development evidence, not immutable same-SHA CI evidence
and not a performance threshold. It must not be compared directly with the
Linux snapshot as a platform score. The authoritative remote Linux/Windows
pair has not been generated and remains required before closing the gate.

Build command:

```powershell
cmake -S . -B build-benchmark-final-v4-windows `
  -G "Visual Studio 17 2022" -A x64 `
  -DGAMENET_BUILD_TESTING=OFF `
  -DGAMENET_BUILD_BENCHMARKS=ON `
  -DGAMENET_BUILD_FUZZING=OFF `
  -DGAMENET_ENABLE_TLS=OFF `
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-benchmark-final-v4-windows --config Release `
  --target gamenet_phase4_benchmark --parallel
```

The three invocation parameter sets are the fixed commands documented in
`docs/development/phase4_benchmark.md`. Raw stdout is stored unchanged in the
adjacent JSON files.

Final-v4 summary:

| Scenario | Local result |
| --- | --- |
| Framing | 10,386,847.758 operations/s; 2,575.474 MiB/s |
| Logic queue | 20,000 accepted, 0 rejected; P99 596,206.300 us |
| Broadcast fanout | 32,768 accepted, 0 dropped; completion P99 435.000 us |
