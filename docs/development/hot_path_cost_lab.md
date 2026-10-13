# HP0 Hot-Path Cost Ledger

`gamenet_hot_path_benchmark` is the HP0 opt-in benchmark suite. It does not add
another server implementation. `tools/run_hot_path_cost.py` drives the existing
Core, Phase 4, queued-runtime, and capacity executables and freezes their raw
semantic JSON in one `gamenet.hot_path_cost.v1` ledger.

The suite is available only when `GAMENET_BUILD_BENCHMARKS=ON`. It is not a
CTest, is not installed, and changes no public API.

## Preregistered matrix

`benchmarks/hot_path_cost_inventory.json` is reviewed input, not generated
output. It fixes the commands, baseline implementation label, primary metric,
guardrails, required cost fields, and HP slices that consume each baseline.

| Scenario | Current baseline | Main coverage |
| --- | --- | --- |
| `framing.callback-copy` | `PacketFramer::push()` callback/copy path | HP1 |
| `network-logic.callback-mutex` | callback + `GameCommandQueue` mutex | HP2/5/6/7 |
| `readiness-send.callback-borrowed` | epoll/IOCP + borrowed `trySend` | HP3/4/5/8 |
| `broadcast.current-owner-tasks` | current owner-grouped tasks | HP4/5/6 |
| `connection-capacity.current` | 10k current connection/storage path | HP3/6/8 |
| `overload-recovery.current` | current hierarchical budgets | HP4/5/6/8 |

Inventory changes invalidate comparison with an earlier ledger unless the
review explicitly treats the change as a new scenario version.

## Schema

Every sample has the same ordered cost keys:

```text
cycles, instructions, llc_load_misses, allocations, copied_bytes,
syscalls, wakeups, generic_posts, handoffs,
latency_p50_us, latency_p99_us, latency_p999_us, queue_age_max_us,
rss_bytes, overload_recovery_ms, shutdown_convergence_us
```

A measured zero is `0`. A counter not observed by the child benchmark or the
configured lab observer is `null` and has a non-empty entry in `unavailable`.
The runner never converts an unknown value into zero.

The current child paths already expose several exact costs, including queued
generic posts/handoffs, latency percentiles, maximum queue age, RSS, overload
recovery, and shutdown convergence. Platform counters, allocation counts,
copy counts, physical syscalls, and physical wakeups come from the fixed-lab
observer when the child does not expose them.

## Observer contract

The fixed lab passes `--observer-command PATH`. For each child sample the
runner invokes:

```text
PATH --cost-output OBSERVATION.json -- CHILD [arguments...]
```

The observer must preserve the child's stdout exactly, return the child's
failure as nonzero, and write one document:

```json
{
  "schema": "gamenet.hot_path_observation.v1",
  "costs": {
    "cycles": 1,
    "instructions": 1,
    "llc_load_misses": 0,
    "allocations": null,
    "copied_bytes": null,
    "syscalls": null,
    "wakeups": null,
    "generic_posts": null,
    "handoffs": null,
    "latency_p50_us": null,
    "latency_p99_us": null,
    "latency_p999_us": null,
    "queue_age_max_us": null,
    "rss_bytes": null,
    "overload_recovery_ms": null,
    "shutdown_convergence_us": null
  }
}
```

The example shows the shape only; a fixed-lab run is incomplete if any
scenario's preregistered `required_costs` remains `null`. Linux observers may
use `perf stat` plus allocation/copy probes. Windows observers may use the
reviewed PMC/ETW equivalent. Observer executable hashes are part of the ledger.

## Laboratory preflight

`tools/preflight_hot_path_cost.py` checks readiness before ten-sample collection.
It reads the actual executing OS, kernel identity and WSL indicators, and checks
the exact source commit and Git status itself. Supplying `--platform linux` or
`--native-runner` cannot turn WSL or a Windows process into native Linux evidence.
The ledger runner performs the same independent host checks.

An inspection without `--probe` starts no observer or benchmark. It writes
`preflight.json` with the missing resources and each scenario's costs that the
observer must supply. It exits nonzero and never reports readiness. For example:

```powershell
python tools/preflight_hot_path_cost.py `
  --platform windows --backend iocp `
  --output-root hot-path-results/preflight-inspection
```

Provide the reviewed observer, all four Release binaries, named runner and
frequency policy on a clean checkout, then add `--probe`:

```powershell
python tools/preflight_hot_path_cost.py `
  --platform windows --backend iocp --commit-sha (git rev-parse HEAD) `
  --runner-id $env:RUNNER_NAME --frequency-policy $env:HP0_FREQUENCY_POLICY `
  --observer-command $env:HP0_OBSERVER `
  --core-executable build-hot-path/benchmarks/Release/gamenet_core_benchmark.exe `
  --phase4-executable build-hot-path/benchmarks/Release/gamenet_phase4_benchmark.exe `
  --capacity-executable build-hot-path/benchmarks/Release/gamenet_capacity_profile.exe `
  --queued-executable build-hot-path/benchmarks/Release/gamenet_multi_io_queued_benchmark.exe `
  --output-root hot-path-results/preflight --probe
```

Linux uses `--platform linux --backend epoll` and the single-config executable
paths from the workflow. Each output directory must be new or empty. Probes
first execute two fresh nonce-bearing children through the actual observer:
one succeeds and one exits nonzero. The observer must preserve stdout byte for
byte, emit a valid observation for success, and propagate failure. Preflight
then runs one real child per scenario with the unchanged inventory arguments,
checks the returned parameters against every preregistered argument, reuses the
existing child semantic validators, and checks merged required costs.
A declarations-only observer, missing counter, conflicting child/observer value,
changed executable, or failed lifecycle check stops preparation immediately.
Malformed observations, duplicate JSON keys, non-finite or oversized numeric
values are rejected with a structured failure rather than accepted as costs.
The 10k connection scenario remains 10k during this check; no shortened load can
pass it. No probe starts while basic prerequisites are missing.

The manual workflow runs this gate before collecting the ledger. The ledger's
own first warmup also rejects missing required costs immediately, so invoking
the runner directly cannot postpone that failure until all ten rounds finish.
Preflight retains its handshake/raw artifacts separately, always sets
`promotion_eligible=false`, and never supplies ledger warmups or recorded
samples. A ready result permits collection; it does not close HP0.

This check exercises the observer rather than accepting a capabilities file.
It does not certify the correctness of an arbitrary observer's instrumentation:
the executable and its allocation/copy/PMC/ETW collection method still need lab
review. The observed host is the process's OS, not proof of a particular physical
machine or an enforced frequency policy. Runner provisioning must separately
pin affinity, establish the reviewed policy, and register online self-hosted
Linux and Windows `x64,gamenet-performance` runners. An observer that only
supplies cycles/instructions/cache misses cannot complete HP0: framing also
requires allocation and copied-byte observations; other scenarios require
syscalls, physical wakeups and costs absent from their child JSON.

Behavioral tests in `tests/cmake/test_hot_path_preflight.py` execute observer
test doubles solely to test forwarding and fail-closed behavior. Their output
is never performance or fixed-lab evidence. The existing HP0 repository contract
runs these tests on its normal CI path.

## Development run

Development evidence is useful for checking commands and schema but cannot
promote a fast path. On Windows with a multi-config generator:

```powershell
cmake -S . -B build-hot-path `
  -DGAMENET_BUILD_TESTING=OFF `
  -DGAMENET_BUILD_BENCHMARKS=ON `
  -DGAMENET_ENABLE_EXPERIMENTAL=OFF
cmake --build build-hot-path --config Release `
  --target gamenet_hot_path_benchmark --parallel

$sha = git rev-parse HEAD
python tools/run_hot_path_cost.py `
  --core-executable build-hot-path/benchmarks/Release/gamenet_core_benchmark.exe `
  --phase4-executable build-hot-path/benchmarks/Release/gamenet_phase4_benchmark.exe `
  --capacity-executable build-hot-path/benchmarks/Release/gamenet_capacity_profile.exe `
  --queued-executable build-hot-path/benchmarks/Release/gamenet_multi_io_queued_benchmark.exe `
  --output-root hot-path-results `
  --platform windows --backend iocp --build-type Release `
  --commit-sha $sha --working-tree dirty --evidence-class development `
  --native-runner --runner-id local-development `
  --cpu-model local --cpu-affinity inherited `
  --frequency-policy unmanaged --compiler msvc `
  --command-context local-smoke --samples 1

python tools/validate_hot_path_cost.py `
  --input hot-path-results/hot-path-cost.json
```

Use `--wsl` for WSL. WSL and dirty-worktree ledgers remain development-only.
The runner independently checks `git rev-parse HEAD` and full porcelain status
under `--source-root` (the current directory by default), so `--commit-sha` and
`--working-tree` are assertions rather than trusted metadata.

Every child invocation also retains its actual argv and byte-for-byte stdout
and stderr in `.command.json`, `.stdout.bin`, and `.stderr.bin` sidecars beside
its observation path. Each scenario has a separate `warmup.observation.*` set;
recorded samples use `sample-NN.observation.*`. Nonzero exits, malformed JSON,
invalid UTF-8, and timed-out partial output remain available for diagnosis.
These sidecars are diagnostic artifacts outside the ledger's indexed hashes;
the existing manifest-indexed sample JSON and observation files remain the
authority for validation and promotion. Retained warmups do not count as samples.

## Fixed laboratory gate

`.github/workflows/hot-path-benchmark.yml` is manual-only and targets named
self-hosted Linux and Windows performance runners. The runner service is pinned
to the reviewed CPU set, so every child inherits the recorded affinity. The
workflow requires a clean checkout, native epoll/IOCP execution, Release,
exactly one unrecorded warmup, at least ten scenario-round-robin samples, and a
lab observer. It validates with `--require-fixed-lab` and retains all raw JSON,
stderr, observation files, manifest, toolchain, and machine information.

Hosted CI runs the repository and workflow contracts only; a hosted runner is
not stable enough to mint performance promotion evidence. A complete Linux
ledger and complete Windows ledger must be indexed against the same exact
commit before HP0 closes. Until that happens, HP1-HP8 promotion, integration,
default-path changes, and installed API additions remain blocked by evidence.

## Experimental prestudy while HP0 is open

HP0 evidence collection may run in parallel with at most one HP1-HP8 runtime
prototype labeled `EXPERIMENTAL-PRESTUDY`. The prototype must be default-off,
non-installed, non-exported, absent from the production Core path and public
manifests, and unable to replace any preregistered current-path baseline.
Design, contracts, and benchmark scaffolding may be prepared out of milestone
order, but a runtime experiment must use real satisfied dependencies and retain
deterministic owner, admission, failure, shutdown, and zero-residue tests.

Prestudy results are development-only. They may support
`KEEP-EXPERIMENTAL`, `REJECT`, or `DEFER`, but cannot produce `INTEGRATE`, a
5%/3% promotion decision, a default switch, an installed API, or a release.
After HP0 closes, a surviving candidate must enter its formal HP slice and be
remeasured against the fixed exact-commit baseline.

## Interpretation

The ledger is a cost baseline, not a threshold by itself. Candidate promotion
uses the preregistered primary metric, a 95% bootstrap confidence interval, the
5% minimum improvement rule, and the 3% non-target guardrails from `plan.md`.
Correctness, ownership, generation, backpressure, recovery, and zero-residue
contracts remain mandatory even when the performance comparison improves.
