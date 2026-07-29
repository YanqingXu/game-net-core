# Core Benchmark Baseline

`gamenet_core_benchmark` is an opt-in loopback benchmark for the Reactor/TCP
core. It is deliberately separate from CTest: correctness gates must not fail
because a shared runner is noisy, and timing numbers must not be interpreted as
portable production capacity.

## Build

Linux Release:

```bash
cmake -S . -B build-benchmark \
  -DCMAKE_BUILD_TYPE=Release \
  -DGAMENET_BUILD_TESTING=OFF \
  -DGAMENET_BUILD_BENCHMARKS=ON \
  -DGAMENET_ENABLE_TLS=OFF \
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-benchmark --target gamenet_core_benchmark --parallel
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
  --target gamenet_core_benchmark --parallel
```

The option defaults to `OFF`. The executable is not registered with CTest and
is not exported or installed with `GameNet::core`.

## Reproducible Scenario Set

Run the following from the repository root. On Linux, replace `$exe` with
`./build-benchmark/benchmarks/gamenet_core_benchmark`.

```powershell
$exe = Resolve-Path build-benchmark-windows/benchmarks/Release/gamenet_core_benchmark.exe

& $exe --scenario echo --connections 4 --threads 1 `
  --messages 10000 --payload 256 --settle-ms 500 --timeout-ms 30000

& $exe --scenario echo --connections 4 --threads 2 `
  --messages 10000 --payload 256 --settle-ms 500 --timeout-ms 30000

& $exe --scenario connections --connections 256 --threads 1 `
  --settle-ms 1000 --timeout-ms 30000

& $exe --scenario connections --connections 10000 --threads 1 `
  --settle-ms 2000 --timeout-ms 120000

& $exe --scenario slow-client --connections 4 --threads 1 `
  --slow-bytes 8388608 --high-water 65536 `
  --settle-ms 1000 --timeout-ms 30000
```

The 10k connection command is the structured idle-memory profile used by
M3-G4; compare `working_set_delta_bytes` and
`approx_bytes_per_connection` against the same-runner baseline. The two echo
commands provide the first one-worker/two-worker scaling point.
They do not alter the poller. Current Windows candidates report
`get_queued_completion_status_ex_batch_64`, while Linux reports
`epoll_wait_batch`. Historical Windows Core Preview artifacts report
`single_get_queued_completion_status`; the distinct value prevents batched
candidate results from being confused with that baseline.

## Scenarios and Measurements

- `echo` creates all clients before timing. Each client performs sequential
  request/echo round trips. `throughput_mib_per_second` counts request bytes
  plus echoed response bytes. P50/P99 use nearest-rank RTT samples in
  microseconds.
- `connections` samples process working set before client creation and after
  all accepted idle connections have settled. `approx_bytes_per_connection`
  is the process working-set delta divided by configured connections, not an
  allocator-level object-size claim.
- `slow-client` gives clients a small receive buffer, holds reads through the
  working-set sample, and calls `trySend()` from each connection owner loop.
  It then drains accepted output to observe write completion and read-throttle
  recovery. The result records requested/accepted/rejected bytes, rejection
  reasons, configured low/high/hard output thresholds and input limit,
  pending-output peak, pause/resume observations, high-water notifications, and
  recovery duration. `--high-water` drives both the connection read-throttle
  threshold and notification threshold; the recorded low-water value is half.

Every current run writes one `gamenet.core_benchmark.v2` JSON document to stdout.
Stable keys include scenario parameters, platform, backend, completion mode,
build type, throughput, P50/P99, connection count, EventLoop worker count,
working-set before/after/delta, approximate bytes per connection,
requested/accepted/rejected admission accounting, configured low/high/hard and
input limits, pending-output peak, throttle observations, recovery duration,
and high-water callback count.
Diagnostics are written to stderr, and setup/I/O/timeout failures return a
non-zero exit code with `status: "error"` when lifecycle-safe reporting is
possible.

The frozen performance baseline still emits `gamenet.core_benchmark.v1`.
The matrix runner accepts v1 only for that reviewed baseline and v2 for the
candidate. Relative regression compares their common reviewed performance
metrics; v2 admission and recovery accounting is validated independently and
is never inferred for v1 samples.

## Evidence Discipline

Raw JSON evidence is the durable record. Keep each JSON document unchanged and
record the command, date, operating system, compiler/build type, backend, and
completion mode beside it. Compare runs only when those fields and scenario
parameters match. CPU load, power policy, allocator state, and loopback stack
noise can materially change results.

The first historical Core v1 Windows Release evidence is stored under
`docs/development/benchmark_results/2026-07-10-windows-msvc-release/`. Linux
and Windows regression infrastructure later passed together in workflow run
`29808395220` at candidate SHA `5f926f3`. That run predates the current v2 and
runtime changes, so it is retained as tooling evidence rather than final
candidate evidence.

## Manual Cross-Platform Capture

The manual-only `core-benchmark` workflow runs the fixed scenario set in Linux
Release and Windows MSVC Release jobs. Each producer builds both the reviewed
baseline and candidate on the same runner, executes three repetitions of an
expanded 1/2/4-worker, 256/1,024-connection, and 4/16-slow-client matrix, and
enforces the reviewed relative budgets. It also preserves the four original
canonical raw JSON artifacts as one bundle. The
canonical artifact name binds the producer job, commit SHA, workflow run id,
and run attempt:

```text
core-benchmark-${{ github.job }}-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
```

Including the run attempt keeps an immutable `upload-artifact@v4` bundle from
colliding with an earlier attempt when the same workflow run is rerun. Record
the workflow run id and attempt, then copy both artifact sets into the evidence
ledger before declaring the cross-platform baseline complete.

The workflow also builds and captures the separate Phase 4 scenario set. Those
documents use `gamenet.phase4_benchmark.v1` and distinct artifact names; see
`docs/development/phase4_benchmark.md`. They do not change this Core schema or
the four-file Core artifact contract.

The full baseline/candidate sample sets and `gamenet.performance_regression.v1`
are retained with the Phase 4 producer evidence. See
`docs/development/performance_regression.md`. These comparisons are same-runner
regression gates, not cross-platform capacity comparisons.
