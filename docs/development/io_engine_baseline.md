# ARCH-G1 I/O Engine Baseline

- Collected: 2026-08-19
- Runtime source checkpoint: `669ebb0a7c5c475dea74b12275c66a2ce1876804`
- Collection worktree start: `65b3051ce97bcf44ee75eb62ec270c2ab38b5abb`
- Scope: local directional comparison for IOE-R1, not release evidence

The commits between the runtime checkpoint and the collection worktree changed
planning/governance documentation, not the benchmarked runtime sources. Both
builds were rebuilt before collection.

## Environments and Commands

Windows used CMake 4.3.1-msvc1, Visual Studio 18 2026, MSVC 19.51.36252.0, and
the existing `build-perf-r1-fix` Release configuration. Linux used WSL Ubuntu,
CMake 3.28.3, GCC 13.3.0, Unix Makefiles, and `build-wsl-perf-r1-fix`.

```text
cmake --build build-perf-r1-fix --config Release --target gamenet_core_benchmark --parallel
build-perf-r1-fix\benchmarks\Release\gamenet_core_benchmark.exe --scenario echo --connections 4 --threads 1 --messages 10000 --payload 256 --settle-ms 500
build-perf-r1-fix\benchmarks\Release\gamenet_core_benchmark.exe --scenario connections --connections 256 --threads 1 --settle-ms 1000
build-perf-r1-fix\benchmarks\Release\gamenet_core_benchmark.exe --scenario slow-client --connections 4 --threads 1 --slow-bytes 8388608 --high-water 65536 --settle-ms 500

wsl bash -lc 'cd /mnt/g/github/game-net-core; cmake --build build-wsl-perf-r1-fix --target gamenet_core_benchmark -j 4'
wsl bash -lc 'cd /mnt/g/github/game-net-core; ./build-wsl-perf-r1-fix/benchmarks/gamenet_core_benchmark --scenario echo --connections 4 --threads 1 --messages 10000 --payload 256 --settle-ms 500'
wsl bash -lc 'cd /mnt/g/github/game-net-core; ./build-wsl-perf-r1-fix/benchmarks/gamenet_core_benchmark --scenario connections --connections 256 --threads 1 --settle-ms 1000'
wsl bash -lc 'cd /mnt/g/github/game-net-core; ./build-wsl-perf-r1-fix/benchmarks/gamenet_core_benchmark --scenario slow-client --connections 4 --threads 1 --slow-bytes 8388608 --high-water 65536 --settle-ms 500'
```

## Numerical Sample

| Scenario/metric | Windows IOCP | Linux epoll |
| --- | ---: | ---: |
| Echo backend mode | GQCSEx, batch 64 | epoll_wait batch |
| Echo throughput | 61.878 MiB/s | 45.686 MiB/s |
| Echo messages/s | 126,725 | 93,564 |
| Echo P50 | 30.9 us | 30.875 us |
| Echo P99 | 53.9 us | 112.45 us |
| Echo P999 | 84.1 us | 234.433 us |
| Echo close / stop | 1.588 / 0.253 ms | 1.363 / 0.132 ms |
| 256 connections establish time | 13.793 ms | 93.740 ms |
| 256 connections establish rate | 18,560/s | 2,731/s |
| Approx. working-set delta/idle connection | 11,856 bytes | 5,264 bytes |
| 1 s idle CPU | 0% | 0.0029% |
| 256 connections close / stop | 6.825 / 0.278 ms | 2.739 / 0.160 ms |
| Slow-client accepted bytes | 33,554,432 | 33,554,432 |
| Slow-client rejected bytes | 0 | 0 |
| Slow-client peak pending output | 8,388,608 | 6,594,560 |
| Slow-client pause / resume / high-water callbacks | 4 / 4 / 4 | 4 / 4 / 4 |
| Slow-client recovery | 33.047 ms | 26.903 ms |
| Slow-client close / stop | 1.156 / 0.189 ms | 1.296 / 0.148 ms |

All six runs reported drained shutdown. The four-connection echo working-set
delta and slow-client working-set delta are allocator/payload-sensitive and are
not suitable as per-connection gates. The 256-idle-connection sample is the
directional memory comparison.

## Behavioral Baseline

- EventLoop is single-owner-thread; cross-thread work is admitted through
  `runInLoop`, `queueInLoop`, wakeup, or registered control sources.
- Dispatch has bounded active-I/O, timer, control, and functor budgets.
- Channel removal invalidates an active-batch registration generation before
  destruction or same-address reuse.
- Shutdown converges through Running, Quiescing, FinalDraining, and Shutdown;
  accepted work drains and new external admission is rejected.
- Windows successful overlapped submissions retain operation storage until the
  real completion packet is dequeued. Cancellation alone does not retire it.
- The current Windows delivery path translates IOCP operation kinds into
  synthetic Channel read/write events. This is compatibility behavior for
  IOE-R1 and explicit debt for IOE-C1.
- The current Release test baseline is 121/121 CTest tests on Windows. The same
  focused I/O/lifecycle contracts must also pass under WSL before IOE-R1 closes.

## IOE-R1 Comparison Rule

The adapter slice must preserve all behavioral items and produce no unexplained
material numerical regression. A change beyond 5% in echo throughput or P99,
idle-connection memory, or shutdown time requires at least three paired runs on
the affected platform and a written accept/fix decision. This trigger is for
investigation; it is not a claim that one noisy local sample is a release gate.

The initial observability gap is also recorded: the current metrics expose an
IOCP packet count but do not expose backend-neutral Engine wait count, delivered
notice count, dispatch lag, or retained-operation count. IOE-R1 must establish
the neutral adapter counters without changing installed metrics semantics.

## IOE-R1 Adapter Preflight

The final adapter worktree was rebuilt on both platforms and compared with a
fresh build of runtime checkpoint `669ebb0a7c5c475dea74b12275c66a2ce1876804`.
Each side received one warmup followed by five alternating echo samples; the
table reports medians.

| Platform | Baseline throughput | Adapter throughput | Delta | Baseline P99 | Adapter P99 | Delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows/IOCP | 51.826 MiB/s | 54.652 MiB/s | +5.45% | 78.9 us | 77.4 us | -1.90% |
| Linux/epoll | 46.879 MiB/s | 45.277 MiB/s | -3.42% | 111.692 us | 116.025 us | +3.88% |

The Windows throughput change is an improvement; both Linux changes remain
inside the 5% investigation threshold. The decision is to integrate this
adapter slice with no accepted regression. These are local preflight results;
the exact integrated commit is recorded separately in the evidence ledger.
