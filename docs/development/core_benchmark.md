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

& $exe --scenario slow-client --connections 4 --threads 1 `
  --slow-bytes 8388608 --high-water 65536 `
  --settle-ms 1000 --timeout-ms 30000
```

The two echo commands provide the first one-worker/two-worker scaling point.
They do not alter the poller: Windows currently reports
`single_get_queued_completion_status`, while Linux reports
`epoll_wait_batch`. A future IOCP batched-drain implementation must use a new
completion-mode value so its results cannot be confused with this baseline.

## Scenarios and Measurements

- `echo` creates all clients before timing. Each client performs sequential
  request/echo round trips. `throughput_mib_per_second` counts request bytes
  plus echoed response bytes. P50/P99 use nearest-rank RTT samples in
  microseconds.
- `connections` samples process working set before client creation and after
  all accepted idle connections have settled. `approx_bytes_per_connection`
  is the process working-set delta divided by configured connections, not an
  allocator-level object-size claim.
- `slow-client` gives clients a small receive buffer, never reads responses,
  and offers `slow_bytes_per_connection` from each connection owner loop. It
  records working-set growth and high-water notifications. The current
  `high_water_notification_only` policy is observable, not a memory cap.

Every run writes one `gamenet.core_benchmark.v1` JSON document to stdout.
Stable keys include scenario parameters, platform, backend, completion mode,
build type, throughput, P50/P99, connection count, EventLoop worker count,
working-set before/after/delta, approximate bytes per connection, offered
bytes, and high-water callback count. Non-applicable measurements are `null`.
Diagnostics are written to stderr, and setup/I/O/timeout failures return a
non-zero exit code with `status: "error"` when lifecycle-safe reporting is
possible.

## Evidence Discipline

Raw JSON evidence is the durable record. Keep each JSON document unchanged and
record the command, date, operating system, compiler/build type, backend, and
completion mode beside it. Compare runs only when those fields and scenario
parameters match. CPU load, power policy, allocator state, and loopback stack
noise can materially change results.

The first local Windows Release evidence is stored under
`docs/development/benchmark_results/2026-07-10-windows-msvc-release/`. Linux
Release output is still required from a Linux host or CI artifact before the
cross-platform PR-0C evidence gate can be called complete.

## Manual Cross-Platform Capture

The manual-only `core-benchmark` workflow runs the fixed scenario set in Linux
Release and Windows MSVC Release jobs for the same commit. Each job validates
schema, status, platform, backend, and build type without imposing timing
thresholds, then uploads four raw JSON artifacts whose names include the commit
SHA. Record the workflow run id and copy both artifact sets into the evidence
ledger before declaring the cross-platform baseline complete.
