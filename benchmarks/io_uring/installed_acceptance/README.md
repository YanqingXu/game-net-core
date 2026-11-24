# Installed io_uring acceptance

The standalone consumer requires the installed Release GameNetCore 0.3.1 package
with `GameNet::core` and `GameNet::experimental_io_uring`. The native Linux job
builds the package from a pinned `main` revision and links both backends from
one install prefix. It runs both on the same host and CPU pair.

- Each trial uses 64 IPv4 loopback TCP connections and 4,096-byte frames, with
  one server EventLoop and one peer thread pinned to separate logical CPUs.
- Steady traffic warms up with 16 echo rounds. It then measures 256 rounds,
  with one echo in flight per peer in each round. The latency is client RTT
  and throughput includes requests and responses.
- Slow close warms up the same way. Peer 0 holds its reads while the server
  admits an 8 MiB burst and requests a graceful stop with output still queued.
  After a 250 ms hold the peer checks the full burst. All peers check EOF and
  return a write half-close. Throughput counts the burst and includes the hold
  and drain. Latency runs from submission to verified receipt for each frame.
- One trial per backend and scenario warms the host. Five scored pairs
  alternate backend order. CPU time covers the timed phase and comes from
  process and EventLoop thread clocks. CPU percentage uses one core as 100%.
  Observed peak RSS is the maximum of the process `VmRSS` and `VmHWM` readings
  after warmup, at the end of the measurement and after teardown. It includes
  both threads. Keeping the readings also captures peaks when a later `VmHWM`
  is lower than an earlier `VmRSS`.

The replay joins each accepted frame to one delivery and each peer to one
graceful close, checks submission and terminal counts for native accept,
receive and send, and verifies the cleanup counters and file descriptor and
thread baselines. The evidence includes raw trial CSVs, build logs, package
archives, the linked binary, a SHA-256 manifest and the reported aggregates.

Download and extract the workflow artifact, then run this command from a
checkout containing this consumer. Replay uses the Python standard library
and can run on Windows or Linux.

```sh
python3 benchmarks/io_uring/installed_acceptance/run_acceptance.py --verify-output /path/to/extracted-artifact
```

## Steady-state CPU attribution

`cpu_attribution.py` keeps the 64-connection workload, extends the scored
steady trials to 4,096 rounds, and runs five interleaved Linux Release pairs.
Dedicated observation trials use 8,192 rounds for `perf` CPU-clock call stacks
and 512 rounds for `strace`. The profiler selects the server thread and the
steady window from raw timestamps. The syscall trace follows the server thread
between its two phase markers. Timing from those observation trials stays out
of the scored baseline. Stack groups count inclusive samples and can overlap.

The isolated candidate applies `callback-send-coalescing.patch` to a second
checkout of the same package commit. The owner can stage one Send while
dispatching a completion. The next accepted operation flushes the staged Send
with its own SQE. Cancellation and the end of dispatch also flush a staged Send.
The experiment compares both installed packages and an epoll reference in
rotating order on one host, then runs five slow-close pairs. The recorded Linux
Release contracts exercise the same experimental archive as the candidate
package with Core's usual test hooks. The timed trials use installed packages
built with testing disabled.

Replay the profile and paired experiment artifacts separately.

```sh
python3 -B benchmarks/io_uring/installed_acceptance/cpu_attribution.py --verify-output /path/to/profile-evidence
python3 -B benchmarks/io_uring/installed_acceptance/cpu_variant.py --verify-output /path/to/experiment-evidence
```

## Completion dispatch budget

The native job first builds and verifies a fresh paired variant on the same
immutable workflow SHA, then uses its exact installed archives and consumer.
The v2 dispatch manifest binds their bytes and the predecessor manifest SHA-256.
The original v1 verifier and `--restore-prior` keep the historical run 45000725413
pin unchanged for archived evidence; fresh runs do not need that expiring artifact.
Every child links the same consumer binary and uses the same pair of pinned
CPUs. The three settings only vary the installed Server's
`IoUringEventLoopPumpOptions::maxNoticesPerTurn`.

- The steady comparison has one warmup block and six scored blocks that
  cover all six orders of 16, 32 and 64. Every trial measures 4,096 echo rounds.
  Reported Pump counters cover the full trial through physical drain and are
  normalized by its successful echoes, including 16 warmup rounds.
- Dedicated syscall traces follow the owner thread through 512 echo rounds.
  The trace includes the full syscall set between the two steady markers.
- Dedicated timer trials run 2,048 echo rounds with a 1 ms owner-loop probe
  on absolute deadlines. The replay independently verifies cadence and
  counts missed deadlines in the workload window. The loop stays live after
  explicitly cancelling the probe to check timer retirement.
- Slow-close trials verify the 8 MiB burst after a 250 ms read hold. Every
  trial checks byte order, one delivery per accepted frame, native operation
  retirement, graceful close, and zero residual resources.

Replay the archive after extraction with the Python standard library.

```sh
python3 -B benchmarks/io_uring/installed_acceptance/dispatch_budget.py --verify-output /path/to/dispatch-budget-evidence
```

## Re-running and identity checks

Installed acceptance and the combined CPU-variant/dispatch sweep run for changes
to this harness on PRs to `main` and pushes to `main`. All three workflows also
support manual dispatch. CPU attribution exposes `variant` (default) and
`profile` (perf stack samples) modes; a dispatch sweep requires `variant`.
Consumer and package checkouts use the same immutable `github.sha`, including
the PR merge commit when validating a PR. No production source is patched.

For an external replay, get the run's checkout SHA independently from Actions
and pass both `--expected-source SHA --expected-consumer SHA`. Replay both CPU
and dispatch artifacts, then compare the CPU manifest's SHA-256 with dispatch's
`predecessor.manifest_sha256`. A self-consistent manifest alone is not an
external attestation of provenance. Preserve downloaded artifacts when long-term
replay is needed (Actions retention is 30 days).

`python3 -B benchmarks/io_uring/installed_acceptance/test_evidence.py` exercises
portable verifier checks. It does not replace native Linux execution, the
11 exact io_uring contracts, sanitizers, or HP0 promotion requirements. These
experiments remain observational, default-OFF, and do not authorize promotion.

## Bounded hot-path before/after comparison

CPU attribution's manual `mode=hot-path` compares the fixed pre-optimization
`e391c2f59e1cd0dc9d5cc8a1cbab215b058c488d` baseline with the dispatched candidate
SHA. It permits runtime differences only in Engine slot bookkeeping and Hub
close-retirement bookkeeping. It does not apply the Send-coalescing patch.

Both packages use the same Release compiler flags, with identical file-prefix
maps for reproducible diagnostic paths. The consumer sources and installed Core
archive must be byte-identical. Each package passes all eleven Release contracts;
the candidate additionally passes all eleven Debug ASan/UBSan contracts. The
tested experimental archives must match the installed ones used for timing.

One excluded warmup block and twelve scored blocks rotate epoll/before/after on
the same two pinned CPUs (64 connections, 4 KiB frames, 4,096 rounds). Three
slow-close blocks per arm retain the 250 ms hold and 8 MiB burst. Separate
1 ms timer probes, perf stacks and syscall traces are never scored as throughput
trials. Notice budget 16, output high-water 64 KiB, per-operation ownership and
physical drain remain unchanged. Paired median percent changes and deterministic
bootstrap intervals describe this one hosted runner, not independent hosts or a
production acceptance decision.

```sh
gh workflow run io-uring-cpu-attribution.yml --ref CANDIDATE_BRANCH -f mode=hot-path -f dispatch_budget=false
python3 -B benchmarks/io_uring/installed_acceptance/hot_path_compare.py \
  --verify-output /path/to/hot-path-evidence --expected-candidate ACTIONS_HEAD_SHA
```

The replay rejects missing/extra trials, wrong identities, changed control builds,
incomplete contracts, WSL host evidence, raw-ledger discrepancies and file-hash
changes. Obtain `ACTIONS_HEAD_SHA` independently from Actions. Performance is
reported even if unchanged or worse; no threshold is weakened and no default
backend, HP0/HP8 status or 24h/72h gate is changed by this experiment.
