# Phase 4 Performance Baseline

`gamenet_phase4_benchmark` is the opt-in Release harness for the active
PacketFramer, LogicLoop, and Broadcast layers. It emits one
`gamenet.phase4_benchmark.v1` JSON document per invocation. The executable is
not a CTest, is not installed, and does not define performance thresholds.

The existing `gamenet_core_benchmark` and its `gamenet.core_benchmark.v1`
schema remain unchanged. Core and Phase 4 results are separate artifacts
because they measure different contracts.

## Build

Linux Release:

```bash
cmake -S . -B build-benchmark \
  -DCMAKE_BUILD_TYPE=Release \
  -DGAMENET_BUILD_TESTING=OFF \
  -DGAMENET_BUILD_BENCHMARKS=ON \
  -DGAMENET_ENABLE_TLS=OFF \
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-benchmark \
  --target gamenet_phase4_benchmark --parallel
```

Windows MSVC Release:

```powershell
cmake -S . -B build-benchmark-windows `
  -G "Visual Studio 18 2026" -A x64 `
  -DGAMENET_BUILD_TESTING=OFF `
  -DGAMENET_BUILD_BENCHMARKS=ON `
  -DGAMENET_ENABLE_TLS=OFF `
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-benchmark-windows --config Release `
  --target gamenet_phase4_benchmark --parallel
```

## Fixed Scenario Set

On Linux, use
`./build-benchmark/benchmarks/gamenet_phase4_benchmark` for `$exe`. On Windows:

```powershell
$exe = Resolve-Path build-benchmark-windows/benchmarks/Release/gamenet_phase4_benchmark.exe
```

Run these commands without changing their parameters when producing the paired
workflow baseline:

```powershell
& $exe --scenario framing --messages 250000 --payload 256 `
  --threads 1 --batch 64 --fanout 1 --tick-us 1000 --timeout-ms 30000

& $exe --scenario logic-queue --messages 20000 --payload 64 `
  --threads 4 --batch 512 --fanout 1 --tick-us 1000 --timeout-ms 30000

& $exe --scenario broadcast-fanout --messages 32 --payload 256 `
  --threads 2 --batch 64 --fanout 1024 --tick-us 1000 --timeout-ms 30000
```

The stable parameter object includes fields that are irrelevant to some
scenarios. For example, `fanout` has no effect on `framing`; it remains present
so artifacts retain one versioned shape.

## Measurements

### Framing throughput

`framing` builds valid wire frames before timing, then decodes them through the
public `PacketFramer` with the configured frame/byte batch budget. It verifies
the exact decoded frame count, payload-byte total, empty internal buffer, and
non-faulted state. `throughput_mib_per_second` counts length prefixes plus
payload bytes; `operations_per_second` counts decoded frames. It is a decode
baseline and does not include encoding or input construction.

### Logic queue lag

`logic-queue` uses `threads` real producer threads while the process main thread
runs the LogicLoop owner EventLoop. `batch` is the maximum commands drained per
tick and `tick-us` is the requested tick interval. The benchmark records
enqueue-to-handler P50/P99, operations per second, accepted/rejected totals,
and queue depth/byte high-water marks. It fails if any configured command is
rejected, lost, left queued, or not handled before timeout.

Timer resolution differs by platform. Compare logic P99 only when the OS,
backend, build/compiler, tick, batch, producer count, payload, and message count
match.

### Broadcast fanout latency and memory

`broadcast-fanout` creates online sessions distributed over `threads` endpoint
owner loops. Each iteration routes and dispatches one shared payload, then waits
for every endpoint send before the next iteration. It reports synchronous route
P50/P99, dispatch-start-to-endpoint queue P50/P99, endpoint route-to-send
P50/P99, full-fanout completion P50/P99, accepted/dropped totals, cumulative
scheduled tasks, per-iteration task high water, and send operations per second.

The endpoint is an in-process benchmark adapter, so latency ends when
`TransportEndpoint::send` executes on the correct owner loop. It does not claim
kernel/TCP delivery latency; the real TCP multi-loop integration contract
remains a correctness test.

A separate sampler thread reads process working set every millisecond during
endpoint/session construction and dispatch. `working_set_peak_bytes` and
`working_set_peak_delta_bytes` are sampled process RSS high-water observations,
not allocator-level object sizes or a broadcast memory cap. The before/after
samples are retained to make sampler interpretation explicit.

## JSON And Failure Contract

Every successful run writes exactly one JSON document to stdout with:

- `schema`, `status`, scenario, platform, backend, and build type;
- the complete fixed parameter object;
- elapsed time and stable nullable measurement keys.

Diagnostics go to stderr. Invalid options, overflow, timeout, rejected/lost
work, wrong decoded totals, or incomplete fanout return non-zero and emit an
error-status document when safe reporting is possible.

`tools/validate_phase4_benchmark.py` applies the same evidence validation on
Linux and Windows. It requires every active-scenario measurement to be finite
and present; operation rates and non-empty work sizes must be positive, while
latencies and RSS delta may validly be zero. It checks P99/P50 ordering and
verifies mathematical count invariants:
framing throughput against decoded operations and wire bytes, Logic accepted
and rejected totals plus queue byte/depth high water, and Broadcast
messages-by-fanout delivery plus per-owner batching and working-set peak/delta
relationships. Rejected/dropped counts must be zero for a successful baseline.
No timing or memory score is compared to a fixed performance threshold.

## Raw JSON Evidence

The manual-only `core-benchmark` workflow builds both benchmark executables and
runs the three Phase 4 commands on Linux epoll and Windows IOCP for the same commit.
It validates schema/status/platform/backend/build type, required metrics, and
count invariants, then uploads three raw JSON documents, toolchain evidence, a
scenario-aware semantic manifest, and a shared CI job manifest per platform.
Artifact names contain the job, actual checkout SHA, workflow run id, and run
attempt. The job manifest enforces checkout/GitHub/current SHA equality and
records candidate/PR identity, runner, exact commands, and SHA-256 for every
evidence file.

A third aggregation-only job runs under `if: always()` so producer failures are
reported rather than hiding the gate. Its first step checks each `needs.*.result`
producer final result and fails unless both producer jobs finished with
`success`. Therefore a manifest written before a later Core or Phase 4 artifact
upload failure cannot satisfy the pair gate. Only after that check does the job
download the Linux and Windows artifacts and run
`tools/verify_phase4_benchmark_evidence_set.py`. The verifier requires both jobs
to share one candidate/checkout/run/attempt identity, verifies all declared file
hashes, semantic-manifest identities, exact Linux/epoll and Windows/IOCP pairing,
the three-scenario set, and identical parameters. Its
`gamenet.phase4_benchmark_pair_evidence.v1` output is the authoritative paired
artifact; two unrelated green platform runs cannot satisfy this gate.
It intentionally does not compare timing or memory scores and does not run on
push or pull requests.

Keep raw JSON artifacts unchanged. Record workflow run id, commit SHA, date,
compiler, runner image, commands, and artifact names in the evidence ledger.
Compare values only across matching fields and parameters. Shared-runner load,
power policy, allocator behavior, timer resolution, and process sampling can
all move the measurements materially.

Local Windows MSVC and Linux Clang Release snapshots are stored under
`docs/development/benchmark_results/2026-07-11-windows-msvc-release-phase4/`
and `docs/development/benchmark_results/2026-07-11-linux-clang-release-phase4/`.
They were refreshed as final-v4 dirty-worktree preflight and passed semantic
validation:

| Environment | Framing | Logic queue | Broadcast fanout |
| --- | --- | --- | --- |
| Windows MSVC / IOCP Release | 10,386,847.758 operations/s; 2,575.474 MiB/s | 20,000 accepted, 0 rejected; P99 596,206.300 us | 32,768 accepted, 0 dropped; completion P99 435.000 us |
| Linux Clang / epoll Release | 38,087,693.105 operations/s; 9,444.046 MiB/s | 452,934.439 operations/s; P99 37,832.501 us | 5,036,891.294 operations/s; completion P99 262.500 us |

These rows came from different execution environments. They prove both harness
backends locally but must not be compared as cross-machine performance scores
and do not replace the still-unproduced paired immutable remote workflow
artifact.
