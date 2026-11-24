# IOE-X1 io_uring Directional Benchmark

The IOE-X1 benchmark drives the Linux-only, default-off raw io_uring Engine
through a finite pipeline of one-shot Send and Recv operations. It is
directional development evidence. It does not replace the production Core
epoll benchmark and is not release or promotion evidence.

Build it explicitly on Linux:

```bash
cmake -S . -B build-io-uring-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGAMENET_BUILD_TESTING=OFF \
  -DGAMENET_BUILD_BENCHMARKS=ON \
  -DGAMENET_ENABLE_EXPERIMENTAL=ON
cmake --build build-io-uring-release --parallel \
  --target gamenet_io_uring_one_shot_benchmark
```

Run one sample with 100,000 round trips, a 256-byte payload, and 64 operations
in flight:

```bash
./build-io-uring-release/benchmarks/gamenet_io_uring_one_shot_benchmark \
  100000 256 64 > build-io-uring-release/io-uring-sample.json
python3 tools/validate_io_uring_benchmark.py --require-release \
  build-io-uring-release/io-uring-sample.json
```

The `gamenet.io_uring_one_shot_benchmark.v1` document reports message and
operation rates, bidirectional MiB/s, P50/P99/P999 round-trip latency,
working-set delta, shutdown latency, accepted/terminal operation counts,
SQ-full rejection, fallback, and residual Engine state. A valid successful
sample has exactly two accepted and terminal operations per round trip, no
fallback or SQ rejection, and no active operation, ready notice, or owned byte
after the drained shutdown.

The peer uses a real finite `SOCK_SEQPACKET` socket. The owner thread keeps at
most the configured depth in flight and consumes every typed terminal notice;
there is no callback, readiness, inline, or unbounded-overflow fallback.

## IOE-X5 shared-Pump TCP Hub benchmark

IOE-X5 adds a separate opt-in benchmark for the fixed-capacity shared Hub. It
uses 256 real loopback TCP connections by default, keeps one 64-byte echo frame
in flight per route for 100 round trips, and validates exact connection,
operation, byte, rejection, and residual-state accounting.

```bash
cmake -S . -B build-io-uring-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGAMENET_BUILD_TESTING=OFF \
  -DGAMENET_BUILD_BENCHMARKS=ON \
  -DGAMENET_ENABLE_EXPERIMENTAL=ON
cmake --build build-io-uring-release --parallel \
  --target gamenet_io_uring_shared_tcp_hub_benchmark
./build-io-uring-release/benchmarks/gamenet_io_uring_shared_tcp_hub_benchmark \
  256 100 64 > build-io-uring-release/shared-hub-sample.json
python3 tools/validate_io_uring_shared_hub_benchmark.py --require-release \
  build-io-uring-release/shared-hub-sample.json
```

The `gamenet.io_uring_shared_tcp_hub_benchmark.v1` document reports round-trip
rate, bidirectional throughput, P50/P99/P999 latency, active working-set delta
and bytes per connection, shutdown latency, high-water marks, rejection
counts, and zero-residue metrics. It is deliberately outside CTest.

Five local WSL Ubuntu 24.04/GCC 13.3 Release samples at 256/100/64 produced
medians of 218,581 round trips/s, 26.682 MiB/s, 1,163/1,270/1,354 us
P50/P99/P999, 3,552 active bytes per connection, and 0.866 ms Hub shutdown.
Five production epoll Core samples at the same connection/message/payload
counts produced 99,317 round trips/s, 12.124 MiB/s, 2,467/4,447/5,682 us
P50/P99/P999, 5,600 bytes per connection, 1.652 ms connection close, and
0.102 ms server shutdown.

These numbers are directional only: the shared-Hub probe uses one poll-based
echo peer, while the Core probe uses the production server/client-worker
topology. They do not establish backend superiority or a regression threshold.

**IOE-X5 decision: `PROMOTE`.** Promotion is narrowly scoped to the next
source-private production-adapter contract-shaping slice. The public backend
selector and production `TcpConnection` remain unchanged until equivalent
cross-backend semantics, failure behavior, and repeatable gates are proven.

## 2026-11-23 installed-package hot-path review

This review concerns the explicit, default-OFF installed io_uring component,
not a production backend promotion. Baseline source is
`e391c2f59e1cd0dc9d5cc8a1cbab215b058c488d`. Work is on the authorized temporary
branch `codex/io-uring-hotpath-optimization`; it is not merged into `main`.

### Lessons applied and measured causes

- [liburing's networking notes](https://github.com/axboe/liburing/wiki/io_uring-and-networking-in-2023)
  distinguish completion-native batching from replacing a readiness notifier.
  The present Pump still submits each one-shot Send/Recv immediately and returns
  to the existing EventLoop; it cannot simply block in submit-and-wait without
  changing the owner-loop/timer contract. No such conversion is made here.
- [The liburing/Photon performance discussion](https://github.com/axboe/liburing/issues/536)
  motivates measuring equivalent work and inspecting CPU/syscall costs before
  drawing a backend conclusion. This experiment freezes the consumer, compiler,
  frame count, connection count, output limits and CPU placement. Historical
  published speedups are not transferred to this project.
- [tokio-uring's design](https://github.com/tokio-rs/tokio-uring/blob/master/DESIGN.md)
  reinforces keeping operation buffers owned through completion/cancellation.
  This change does not pool or borrow in-flight payloads, remove Adapter
  admission ordering, or turn cancel completion into target-operation retirement.

The fresh native baseline profile
[run 45322816978](https://github.com/YanqingXu/game-net-core/actions/runs/45322816978)
was downloaded and independently replayed (raw ledgers, file SHA-256, package
and consumer SHA, stacks and syscall traces). In its owner-only measured window:

- 32,768 echoes submitted 65,536 SQEs through 65,536 `io_uring_enter` calls:
  one SQE per enter, two enters per echo. The epoll arm also made two data calls
  per echo (`readv` plus `sendto`), so "two enters" alone does not explain the gap.
  The io_uring path additionally retains ring/CQE bookkeeping and Pump wakeups.
- 1,000 of 2,157 io_uring owner CPU samples contained ring-submission frames;
  118 contained allocation/copy frames, versus 8 of 1,253 epoll samples.
  These are inclusive, overlapping stack groups, not disjoint time percentages
  or proof that all time beneath `io_uring_enter` is avoidable overhead.
- Source inspection found two unnecessary linear scans: Engine allocation
  searches the operation table from slot zero for every operation; Hub scans
  the connection table after every terminal notice even when no route is
  closing. The before/after trial measures the two removals together, not an
  individual attribution for each change.

Remaining allocation/zeroing/copies, Adapter mailbox/credit bookkeeping, kernel
request processing, CQ retirement and continuation/wakeup costs are still present.
The historical Send-coalescing patch is left as a separate experiment. Multishot,
provided buffers, registered files, SQPOLL and zero-copy remain deferred.

### Implementation and contract review

`IoUringCompletionEngine.cc` now uses a fixed owner-local intrusive free-slot
chain. Successful admission pops once; rejected admission does not pop. Only
`takeNextNotice()` for the exact generation pushes a slot back. CQE decoding
and cancel acknowledgements do not release it. Payload vectors and leases move
to the notice unchanged, and retained notices can outlive later slot reuse.

`IoUringTcpConnectionHub.cc` counts Closing routes and skips its retirement scan
only when the count is zero. The first Closing transition increments once;
final retirement decrements before publishing callbacks. Existing scan order,
consumer-depth guard and physical stop publication remain intact. Reentrant
callbacks can close a neighbor, reuse a slot and close the new generation.

Both changes are owned by the existing EventLoop thread. Neither introduces a
cross-thread entry, allocation pool, background thread or additional admission
capacity. Adapter foreign commands still use the existing bounded marshal path.
The public headers and API manifests are unchanged.

Direct new regression coverage:

- `test_io_uring_completion_engine.cpp`: exhaust the complete operation table,
  hold all decoded notices, reject admission until notices are taken, reuse all
  slots for 16 generations while retaining old payloads/leases, reject stale
  cancellation identities, then drain to zero.
- `test_io_uring_tcp_connection_hub.cpp`: close the sole Closing route, re-enter
  its callback to close a neighbor and a replacement generation, reject a
  repeated close, and verify exactly three retirements and zero remaining bytes
  and native operations.

The defense-oriented static review covered operation identity routing, payload
and lease transfer, cancellation, reentrant route retirement, graceful shutdown
and bounded output admission. No confirmed new security/lifecycle defect was
found in this delta. It did not audit application authentication/business state,
all kernel versions, or deployment-specific adversarial traffic. Normal runtime
contract tests and benchmarks are separate from that static review.

### Reproducible validation

Use `hot_path_compare.py` as described in the
[installed acceptance README](../../benchmarks/io_uring/installed_acceptance/README.md).
Its manifest fixes the baseline and binds the candidate to the independently
obtained Actions SHA. Both Release builds use identical compiler flags and file
prefix maps; their installed Core archives and consumer source files must be
byte-identical. Twelve rotating scored blocks follow one excluded warmup block;
each has 64 connections, 4 KiB frames and 4,096 measured echo rounds. Timer probes
and perf/strace observations are separate, unscored trials.

The native workflow also requires all eleven contracts in both Release builds
and all eleven candidate Debug ASan/UBSan contracts, exact frame/operation
retirement, graceful slow-reader drain, bounded callback depth, fixed notice
budget 16, zero residual resources and independent artifact replay.

The local five-pair WSL development check was directionally positive, but is
not substituted for native evidence. Run 45324078225 failed before any build or
measurement because the newly added intent guard lacked its fixed migration
source checkout; the workflow was corrected to provide the same immutable
`mini_trantor` revision used by main CI, without skipping the guard.

### Native result: modest improvement, not epoll parity

[Run 45324232543](https://github.com/YanqingXu/game-net-core/actions/runs/45324232543)
succeeded on native GitHub-hosted Ubuntu 24.04. Candidate and consumer SHA:
`467e96c5e1cbd77b2739f6b2195ee4187b97d585`; runtime source was introduced by
`99fe9deefec6a4382092cb6ab4ae00c00a792779` and did not change in the workflow fix.
Host: Intel Xeon 6975P-C, kernel `7.0.0-1014-aws`, GCC 13.3.0, CMake 3.31.6;
owner/peer pinned to CPUs 0/1 on distinct physical cores.
The following numbers are all from that single before/after run, not the earlier
profile or a different runner. Throughput counts both directions.

| Median, 12 scored blocks | epoll reference | io_uring before | io_uring after |
| --- | ---: | ---: | ---: |
| Throughput, MiB/s | 1581.693 | 969.285 | 1011.566 |
| P99 round trip, us | 193.759 | 319.447 | 297.908 |
| Owner CPU, ns/echo | 4755.314 | 8056.327 | 7721.177 |
| Peak RSS, MiB | 19.455 | 20.133 | 20.141 |
| Steady stop, ms | 0.766 | 0.710 | 0.709 |

Within-pair median changes (not ratios of the table's independent medians):

- Throughput: **+4.270%**, bootstrap 95% interval **[+3.701%, +5.432%]**.
- Owner CPU per echo: **-4.095%**, interval **[-5.117%, -3.656%]**.
- Round-trip P99: **-6.301%**, interval **[-8.233%, -5.658%]**.

All twelve pairs improved on each of these three metrics. These intervals
resample one runner's blocks, not independent machines or kernels. The optimized
implementation still has approximately **36.0% less throughput**, **62.4% more
owner CPU per echo**, and **53.8% higher P99** than the unchanged epoll reference.
This is evidence for removing bookkeeping costs, not evidence of backend parity.
The paired syscall observations still submit exactly 65,536 SQEs in 65,536
enters on each arm for 32,768 echoes. The optimization does not claim a syscall
reduction, less copying, or a newly batched data path.

Slow-close before/after median stop was 255.700/255.508 ms, including the fixed
250 ms peer read hold. Every trial retained exact delivery and zero residue;
neither dropping accepted output nor shortening the hold produced the gain.

The separate 1 ms timer probe's median P99 was 211.945/209.924 us. However, its
**worst observed delay increased from 243.460 to 858.374 us** across the three
trials. No scheduled tick was missed, but this tail outlier must not be hidden
behind the improving median. The current data cannot distinguish hosted-runner
scheduling noise from a reproducible tail regression. Repeat timer-tail tests on
a controlled native host before any latency-sensitive promotion decision.
The 858.374 us value occurred in the first optimized timer trial; its next two
maxima were 242.197 and 224.446 us. All three baseline maxima were below 244 us.

Validation completed: 23 portable evidence checks; exact 11/11 baseline Release,
11/11 candidate Release and 11/11 candidate Debug ASan/UBSan contracts; all 54
untraced workload/timer trials plus four separate perf/strace observations; native
independent replay of identities, build equivalence, frame ledgers and file hashes.
After download, the repository verifier replayed the extracted artifact locally
with the Actions candidate SHA supplied independently and also passed. WSL was
used only to run this offline replay, not to substitute new performance samples.

Raw artifact:
[io-uring-hot-path-45324232543-1](https://github.com/YanqingXu/game-net-core/actions/runs/45324232543/artifacts/14889271842),
390 files, archive SHA-256
`719c26265b5def23fc0d220249543689eac058214efb2e70a6527bf698ac753a`.
Extracted `manifest.json` SHA-256:
`dafe41279fa25f65ebd35e26b379af7e6b31617f3b8dcaa6990e54eee00aff87`.
Actions retention is 30 days. This document does not constitute a complete HP0
ledger, a cross-host capacity matrix, a 24h/72h endurance result or approval to
switch the production backend. No such gate was changed or run here.
The follow-up audit commit changes only this document; implementation, tests,
consumer, verifier and workflow remain identical to the successful Actions SHA.
