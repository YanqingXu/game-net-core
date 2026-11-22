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

The native job restores the exact two coalesced Release archives from Actions
run 45000725413 after recreating the installed layout from the pinned sources.
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
