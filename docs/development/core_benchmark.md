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

& $exe --scenario connections --connections 4096 `
  --connect-concurrency 64 --iocp-accept-depth 4 --threads 1 `
  --settle-ms 0 --timeout-ms 30000

& $exe --scenario connections --connections 128 `
  --connect-concurrency 64 --iocp-accept-depth 4 `
  --preload-before-loop 1 --threads 1 `
  --settle-ms 0 --timeout-ms 30000

& $exe --scenario connections --connections 1000 --threads 1 `
  --settle-ms 2000 --timeout-ms 120000

& $exe --scenario connections --connections 10000 --threads 1 `
  --settle-ms 2000 --timeout-ms 120000

& $exe --scenario slow-client --connections 4 --threads 1 `
  --slow-bytes 8388608 --high-water 65536 `
  --settle-ms 1000 --timeout-ms 30000
```

The M3-P1 small-echo capacity seed expands payloads 64, 256, 1024 bytes across
1/2/4 worker loops while keeping one common 16-connection, 2,000-message load:

```powershell
foreach ($payload in 64, 256, 1024) {
  foreach ($workers in 1, 2, 4) {
    & $exe --scenario echo --connections 16 --threads $workers `
      --messages 2000 --payload $payload --settle-ms 500 --timeout-ms 60000
  }
}
```

The M3-P1 sustained-churn seed uses a paced 100-connection batch with 16
persistent client connector workers. Five seconds at 1,000 attempts/second
produces exactly 5,000 attempts:

```powershell
& $exe --scenario connection-churn --connections 100 `
  --connect-concurrency 16 --iocp-accept-depth 32 --threads 4 `
  --churn-rate 1000 --churn-duration-ms 5000 `
  --churn-connect-timeout-ms 1000 `
  --settle-ms 0 --timeout-ms 60000 > build-benchmark-windows/churn-1k.json

python tools/validate_core_benchmark.py `
  --input build-benchmark-windows/churn-1k.json `
  --scenario connection-churn --require-zero-churn-failures
```

The 10k connection command is the structured idle-memory profile used by
M3-G4; compare `working_set_delta_bytes` and
`approx_bytes_per_connection` against the same-runner baseline. For M3-P1,
run the same sequential-connect, one-worker command at both 1k and 10k and
also compare
`connection_establish_per_second`, `idle_process_cpu_percent`,
`connection_close_seconds`, and `server_stop_seconds`. The idle CPU interval is
exactly the measured settle window; benchmark completion uses a coalesced
base-loop executor notification, so no recurring coordination timer wakes the
process during that window. Keep this capacity profile distinct from the
explicit `--connect-concurrency` accept-burst profile below; burst-handshake
convergence is not an idle-resource measurement. The two echo
commands provide the first one-worker/two-worker scaling point.
They do not alter the poller. Current Windows candidates report
`get_queued_completion_status_ex_batch_64`, while Linux reports
`epoll_wait_batch`. Historical Windows Core Preview artifacts report
`single_get_queued_completion_status`; the distinct value prevents batched
candidate results from being confused with that baseline.

The 4,096-connection command is the M3-G5 accept-burst profile. It starts 64
client connect workers together, explicitly records the fixed AcceptEx depth,
and uses zero settle delay, so
`elapsed_seconds` measures connection creation through convergence of all
server connection callbacks plus the immediate working-set sample. Compare
alternating same-runner Release medians only; the sequential default remains
one connect worker so existing canonical scenarios do not silently change.
After the sample, connection-scenario clients use abortive close to keep
client-side `TIME_WAIT` accumulation from contaminating repeated burst runs.
The 128-connection preload variant is the lower-noise accept-drain profile: all
client handshakes finish before the base EventLoop starts, and
`elapsed_seconds` begins immediately before loop dispatch. This isolates
queued AcceptEx completion/backlog drain from concurrent SYN creation jitter.

For the M3-H2-D decision, the Windows MSVC Release worktree ran seven preloaded
128-connection samples at 64-way client concurrency, AcceptEx depth four, zero
settle delay, and 1/2/4 worker loops. Median ready-backlog drain was
1.597/1.586/1.567 ms. Five successful live 1,024-connection samples per worker
count reported 1.519/2.014/2.016 s medians (the one-worker profile needed one
retry). Adding workers did not improve the live burst, while the isolated
ready backlog already drained roughly three orders of magnitude faster. This
does not identify the base loop as the bottleneck; it identifies live
client/kernel handshake timing as the dominant uncontrolled interval on this
runner. The project therefore does not add a per-worker accept topology from
this evidence. Linux/epoll `SO_REUSEPORT` remains conditional on a future M3-P1
churn profile that measures a sustained ready backlog and base-loop saturation.

## Scenarios and Measurements

- `echo` creates all clients before timing. Each client performs sequential
  request/echo round trips. One completed RTT is one message, so
  `messages_per_second` is the exact completed RTT count divided by the common
  timed interval. `throughput_mib_per_second` counts request bytes plus echoed
  response bytes over that same interval. P50/P99/P999 use nearest-rank RTT
  samples in microseconds. The strict validator recomputes both rates, proves
  configured RTT and byte counts, and requires ordered percentiles.
- `connections` samples process working set before client creation and after
  all accepted idle connections have settled. `approx_bytes_per_connection`
  is the process working-set delta divided by configured connections, not an
  allocator-level object-size claim. `connection_establish_seconds` and
  `connection_establish_per_second` stop before the idle interval;
  `idle_observation_seconds`, `idle_process_cpu_seconds`, and
  `idle_process_cpu_percent` quantify process CPU while all connections remain
  open and inactive. `connection_close_seconds` ends only after disconnect
  callback counts and the base connection map converge. `server_stop_seconds`
  then covers `stopGracefully()` through worker join, while
  `server_stop_outcome`, initial count, and forced count prove the typed
  termination result. `--connect-concurrency` optionally creates clients from
  a bounded simultaneous worker set for an explicit accept-burst profile; it
  defaults to one. `--iocp-accept-depth` applies the public bounded TcpServer
  option and defaults to four. Connections-only
  `--preload-before-loop 1` waits for all client sockets before starting the
  base loop and measures only the subsequent callback drain.
- `connection-churn` derives an exact total attempt count from
  `--churn-rate` and `--churn-duration-ms`. `--connections` is the maximum
  live batch; a persistent bounded connector pool paces each batch from one
  monotonic start time, waits for cumulative accepts, closes clients
  abortively, and waits for cumulative disconnects before reusing capacity.
  Each client attempt uses nonblocking connect completion and the explicit
  `--churn-connect-timeout-ms` deadline, so a stalled client generator becomes
  an accounted failure instead of an unbounded blocking `connect()`.
  Results report attempted, accepted, connect-failed, and closed counts;
  attempt/accept/close rates over one common interval; sorted per-owner-loop
  accept counts; worker skew `(max - min) / mean`; and per-batch connect,
  ready-accept callback, close, and schedule-lag P99/max. The phase split keeps
  client handshake delay separate from a ready backlog that the base loop has
  not yet turned into callbacks. A fixed-key close-reason object accounts for
  every terminal callback. The strict validator proves all count/rate/skew,
  close-reason, and percentile/max identities, and
  `--require-zero-churn-failures` promotes connect failures to a capture
  failure. Final close and typed graceful-stop fields retain the common
  lifecycle contract.
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
build type, message rate, throughput, P50/P99/P999, connection count,
EventLoop worker count,
precise connection establishment/rate, idle process CPU, connection close and
typed graceful-stop evidence, working-set before/after/delta, approximate bytes per connection,
paced churn attempt/accept/close rates and worker skew,
requested/accepted/rejected admission accounting, configured low/high/hard and
input limits, pending-output peak, throttle observations, recovery duration,
and high-water callback count.

For compatibility, `elapsed_seconds` retains its historical connections
meaning and includes the settle interval. Capacity analysis should use the new
phase-specific fields instead of subtracting configured time from that legacy
aggregate. All CPU/RSS values are process-level observations, not per-object
allocator accounting or portable production thresholds.
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

M3-P1 adds a separate `core-capacity` profile without changing that frozen
release-regression set. Each Linux and Windows producer builds capacity
baseline `bbcdd8af2e736d8f8ed53d49e787f14d7f7cb043` and the candidate on the
same runner, then runs three matching repetitions of 1k/10k idle,
64/256/1,024-byte echo at 1/2/4 workers, and sustained 1k/s churn. The paired
evidence verifier rejects cross-platform parameter drift and retains both raw
sets plus `core-capacity-regression.json`.

Only the Linux candidate churn samples feed
`tools/evaluate_core_accept_topology.py`. Its structured result can recommend
an isolated `SO_REUSEPORT` experiment when the target rate is missed and the
ready-accept phase is demonstrably dominant and saturated. Otherwise it records
`retain_single_listener`; the workflow never enables per-worker listeners by
itself.
