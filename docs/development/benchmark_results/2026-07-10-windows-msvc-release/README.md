# Windows MSVC Release Baseline — 2026-07-10

- Host: Windows 10.0.19045, Intel Core i5-10400F @ 2.90 GHz
- Compiler: MSVC 19.51.36246, Visual Studio 18 2026 generator, x64 Release
- Backend: IOCP
- Completion mode: `single_get_queued_completion_status`
- Source base: `0d61658ad19e8758dbf8119a3444a587e7a54a5a`
- Source state: dirty local worktree containing the Phase 3.5 fixes and the
  benchmark implementation; this is a local baseline, not remote validated-commit evidence
- Transport: IPv4 loopback

| Scenario | Worker loops | Key result |
| --- | ---: | --- |
| echo, 4 connections, 40,000 RTTs, 256 B | 1 | 51.979 MiB/s, P50 35.6 us, P99 61.7 us |
| echo, 4 connections, 40,000 RTTs, 256 B | 2 | 73.994 MiB/s, P50 23.7 us, P99 59.2 us |
| 256 idle connections | 1 | 18,243,584 B working-set delta, about 71,264 B/connection |
| 4 slow clients, 8 MiB offered each | 1 | 67,465,216 B delta, 4 high-water callbacks |

The two-worker echo point delivered about 1.42x the one-worker application-byte
throughput in this single run. That ratio is a local observation, not a stable
scaling guarantee. In the slow-client run, the 67.5 MB process working-set
increase for 33.6 MB offered data is consistent with extra buffering in the
current Windows write path; it demonstrates why high-water notification alone
must not be described as a memory bound.

Raw JSON files in this directory preserve the exact executable output. See
`docs/development/core_benchmark.md` for commands and interpretation limits.
