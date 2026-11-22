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
Release io_uring contracts exercise the same archives as the candidate package.

Replay the profile and paired experiment artifacts separately.

```sh
python3 -B benchmarks/io_uring/installed_acceptance/cpu_attribution.py --verify-output /path/to/profile-evidence
python3 -B benchmarks/io_uring/installed_acceptance/cpu_variant.py --verify-output /path/to/experiment-evidence
```
