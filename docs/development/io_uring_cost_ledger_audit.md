# Native per-frame cost ledger and poll-first experiment

## Decision

Keep the existing runtime. The single-factor `IORING_RECVSEND_POLL_FIRST`
experiment did not establish a repeatable throughput or owner-CPU improvement:
the median paired changes were **+0.28% throughput** and **-0.26% CPU per echo**,
within the contemporaneous epoll control variation. One of five pairs reversed
direction. Retain the reproducer and negative result; do not enable the hint in
the library, merge `main`, change the default backend, or claim production
readiness.

This follow-up starts from frozen runtime baseline
`6004293e0492819e062326032a5391eda6e83229`, which already contains the
[precise-wait timer fix](io_uring_timer_tail_audit.md). Tested observer revision
is `7b7fa16e531ad38c125826f0c2d55c8a2deafcc9`. Its committed `src`, `include`,
`cmake`, `api`, and root `CMakeLists.txt` are unchanged from that baseline.
Only the benchmark's temporary clone receives the retained one-line patch.
This audit is a later documentation-only commit, not a newly benchmarked runtime.

## Equal-condition experiment

[Native comparison 45369498548](https://github.com/YanqingXu/game-net-core/actions/runs/45369498548)
completed successfully on GitHub-hosted Ubuntu 24.04.5, Xeon 6975P-C, kernel
`7.0.0-1014-aws`, GCC 13.3.0, CMake 3.31.6. Owner and peer were pinned to CPUs
0 and 1 on distinct physical cores. Neither WSL data nor scores from an earlier
runner are substituted for either arm.

- Both arms use installed Release packages, the same consumer, compiler flags,
  64 connections, 4 KiB request/reply frames, socket settings and affinity.
  Debug-path normalization makes the two installed Core archives byte-identical.
- Fixed-work throughput/CPU measurements use 4,096 rounds and five rotated,
  interleaved blocks, after a separate four-arm warmup block: 20 scored trials
  and four warmup trials.
- Fixed-duration timer observations use a 1 ms timer and five interleaved blocks
  of saturated and 1,000 us paced traffic, four arms each: 40 trials. Each runs
  for at least 2 seconds and finishes the outstanding round. Actual measured
  durations are 2,000.007--2,001.399 ms. Capacity is preallocated; exhaustion is
  an error rather than silently shortening the observation.
- Twelve additional diagnostic trials are not scores: perf uses 8,192 rounds,
  strace 512 rounds, and allocation/copy observation 1,024 rounds for each arm.
  Four separate slow-close trials exercise pending bytes and physical stop.
- The hypothesis changes only Recv SQE `ioprio` by adding
  `IORING_RECVSEND_POLL_FIRST`. The
  [liburing recv manual](https://man7.org/linux/man-pages/man3/io_uring_prep_recv.3.html)
  describes skipping the initial receive attempt and polling first when the
  socket is expected to be empty. It is a hint, not a performance guarantee.
  The experiment requires Linux >=5.19; the library gains no new requirement.

## What the cost ledger establishes

The separate baseline strace window contains 32,768 `io_uring_enter` calls,
each requesting two SQEs: 65,536 SQEs for 32,768 echoes. The after window has the
same histogram. Thus the measured steady path is already **two SQEs per enter,
one enter per echo**, not the old single-SQE-submit pattern. This does not claim
that every lifecycle or cancellation path batches identically.

The baseline io_uring owner perf window has 2,005 samples: 1,684 kernel (83.99%),
237 user (11.82%), and 84 unknown (4.19%). Disjoint nearest-stack-context bins,
as percentages of all samples, are:

| Context | Baseline | Poll-first |
| --- | ---: | ---: |
| Kernel TCP/IP | 60.30% | 58.78% |
| Kernel io_uring | 22.54% | 24.61% |
| Kernel poll/schedule | 0.95% | 1.11% |
| Other kernel | 0.20% | 0.25% |
| Adapter | 6.03% | 5.11% |
| Hub | 2.19% | 2.23% |
| Engine | 1.10% | 0.61% |
| Pump | 0.75% | 0.66% |
| Core | 0.05% | 0.20% |
| Allocation/copy leaf | 1.50% | 1.87% |
| Other user | 0.20% | 0.35% |
| Unknown | 4.19% | 4.20% |

The after window has 1,975 samples. These are sampling/stack-context categories,
not exact exclusive function costs or a causal decomposition of the throughput
gap. Inclusive callers such as `Pump` must not be added to their callees. A
Consumer function taking an Adapter parameter is not classified as Adapter
execution. This observer measures **on-CPU** execution, not runnable delay or
blocked time; wall time minus thread CPU is not a scheduler attribution. The
earlier matched scheduler-event evidence remains separate.

The isolated allocation/copy build records the following owner-side lower bounds
per completed 4 KiB echo:

| Observed operation | epoll baseline | io_uring baseline |
| --- | ---: | ---: |
| C++ allocation calls | 0.000015 | 5.7291 |
| Requested allocation bytes | 0.00061 | 16,801.99 |
| Explicit memcpy calls / bytes | 1 / 4,096 | 4 / 16,384 |
| Explicit memset bytes | 0.74 | 4,095.50 |

Symbolized call sites identify the common consumer frame-assembly copy and the
additional Adapter admission, Hub payload and Engine send snapshots. Recv buffer
resize accounts for roughly 4 KiB of initialization. Poll-first leaves these
counts effectively unchanged. The extra 12 KiB of explicit copies is real, but
calling all four copies redundant would ignore ownership and consumer semantics.

The bounded owner-only TLS observer overrides C++ new/delete and wraps explicit
memcpy/memmove/memset in a separate non-PIE, no-builtin diagnostic build. It does
not count direct malloc, prebuilt DSO internals or compiler-inlined copies.
Allocation bytes are traffic, not resident memory. Its handshake window can
include residual warmup completions. Counts therefore do not imply exact
full-lifetime per-operation counts, and 1.50% sampled allocation/copy leaves do
not bound indirect cache/page effects. Neither diagnostic timings nor instrumented
CPU totals are mixed into the scored results.

## Throughput and CPU result

Absolute values below are medians of five uninstrumented fixed-work trials.
RTT P99 is the median of per-trial P99 values, not a pooled percentile.

| Arm | Throughput, MiB/s | Owner CPU, ns/echo | RTT P99, us |
| --- | ---: | ---: | ---: |
| epoll before | 1,564.851 | 4,807.127 | 196.527 |
| epoll after control | 1,563.465 | 4,808.128 | 194.946 |
| io_uring before | 1,024.214 | 7,627.133 | 288.601 |
| io_uring poll-first | 1,029.875 | 7,585.491 | 283.677 |

The median of the five **within-block percentage changes** is a different
statistic from the ratio of the two absolute medians:

| Paired change | io_uring | epoll control |
| --- | ---: | ---: |
| Throughput | +0.279% | -0.702% |
| Owner CPU per echo | -0.256% | +0.554% |
| RTT P99 | -1.504% | -0.028% |

io_uring throughput changes by block are +0.867%, +2.542%, +0.263%, +0.279%,
-0.116%; epoll ranges from -1.009% to +2.146%. The small headline gain is not a
demonstrated optimization, and no statistical confidence claim is made from
five short samples. The baseline io_uring throughput is still about 34.5% below
epoll in this particular saturated loopback workload; its owner CPU cost is
about 58.7% higher. This is not a general comparison for all traffic shapes.

## Timer-tail recheck

These are separate equal-duration windows. P99 columns are medians of five
per-trial P99 values. Worst columns retain the maximum across all five trials;
missed ticks are summed, not hidden behind a zero median.

| Scenario | Before P99, us | After P99, us | Before worst, us | After worst, us | Before / after missed ticks |
| --- | ---: | ---: | ---: | ---: | ---: |
| Saturated epoll | 106.000 | 109.629 | 2,187.282 | 194.453 | 4 / 0 |
| Saturated io_uring | 211.340 | 200.218 | 2,936.433 | 270.701 | 5 / 0 |
| Paced epoll | 110.770 | 126.360 | 764.269 | 2,766.706 | 0 / 7 |
| Paced io_uring | 131.964 | 124.478 | 3,238.098 | 243.488 | 10 / 0 |

Each row/arm has about 10,000 scheduled ticks (10,000--10,003). Millisecond
outliers remain, including in unchanged epoll. These untraced outliers cannot be
assigned to scheduler delay or claimed fixed by poll-first merely because an
after window missed them. The preceding precise-wait fix is preserved, but this
bounded follow-up is neither a new timer-tail fix nor a production latency SLO.

## Contract review and verification

Defensive static review was limited to this experiment's admission, ownership,
callback and retirement boundaries. Owner affinity, independent Accepted payload
storage, FIFO mailboxes, one Recv/one Send per route, and physical stop remain
unchanged. Cancellation CQEs remain bookkeeping; target terminal plus notice
consumption still governs slot reuse. No accepted-byte backpressure threshold,
slot/byte bound, callback domain or lifecycle phase is relaxed. Pump stays at 16
notices; Core phase budgets remain 64 / 1,024 / 64 / 1,024 / 1,024. This is not a
comprehensive security audit.

- All 11 native io_uring/cross-backend contracts pass in baseline Release,
  poll-first Release and poll-first ASan/UBSan: 33 executions, no skips/failures.
  They cover Engine, Pump, Driver, Hub/capacity, Listener, Adapter, Server,
  MultiOwnerServer, Client and cross-backend TCP semantics.
- All four slow-close trials deliver 3,072 accepted frames to 64 peers with EOF,
  balanced terminal accounting and zero final native residue. io_uring has
  8 MiB pending at stop and completes in 255.485 / 255.728 ms with the existing
  250 ms peer hold. Pending-at-stop is deliberately nonzero, not leaked residue.
- Ten new observer/verifier tests, 30 acceptance-evidence tests and nine
  timer-evidence tests pass. Scope guard passes. WSL diagnostic builds and
  smoke/replay checks are development validation only, never scored evidence.
- [CI 45369504616](https://github.com/YanqingXu/game-net-core/actions/runs/45369504616)
  is successful at the same tested SHA: Linux Debug, Release, ASan/UBSan, TSan,
  Windows MSVC Debug/Release, and six-producer aggregation. The six downloaded
  producer artifacts independently pass `tools/verify_ci_evidence_set.py`,
  including run/attempt/SHA, inventories and file hashes.
- The downloaded native artifact independently passes `cost_ledger.py` replay:
  all manifest hashes, raw scores, diagnostic inventories, build flags, native
  host identity, budgets, contract XML, and the exact one-line derived Engine
  transformation. Its 216 baseline and 223 candidate archived source blobs also
  match the independently known Git trees, not just artifact-declared SHAs.

## Evidence identity and unsuccessful attempts

Primary artifact:
[io-uring-cost-45369498548-1](https://github.com/YanqingXu/game-net-core/actions/runs/45369498548/artifacts/14910145066).
Run attempt 1; ZIP size 333,968,450 bytes. ZIP SHA-256 matches GitHub's
authoritative artifact digest:
`d9ad47dfdb7ed1fc7646a64236060b44c7b0d45f7c4d9f9cd1271f7208445caa`.
Manifest SHA-256:
`93f782c0193e7bcc12e171fdc54450d61c6b35834a46096b6ab9915c7a3ce413`.
The ZIP, extracted evidence and independent CI replay are retained in the task
workspace; Actions artifact retention is 30 days.

Before selecting this hypothesis, baseline
[profile 45366237042](https://github.com/YanqingXu/game-net-core/actions/runs/45366237042)
at `6004293` passed independent replay and already showed two-SQE submission and
predominantly kernel on-CPU samples. Its absolute scores are not the final
comparison's before arm.

The first cost diagnosis,
[45367546981](https://github.com/YanqingXu/game-net-core/actions/runs/45367546981),
passed on-runner replay but its downloaded artifact omitted three archived
`.github/workflows` files. Downloaded-manifest verification correctly rejected
it; it is not accepted as a complete evidence set. The final workflow explicitly
includes hidden files only under its generated evidence directory. No missing
files were reconstructed inside the rejected download.

[CI 45368912910](https://github.com/YanqingXu/game-net-core/actions/runs/45368912910)
failed the scope guard because synthetic test symbols omitted the actual
`gamenet::experimental::io_uring` namespace. Fixtures were corrected without
weakening the guard. Superseded comparison
[45368903294](https://github.com/YanqingXu/game-net-core/actions/runs/45368903294)
was cancelled after the export issue was found. Neither run is counted as green;
both issues are covered by the final successful runs at `7b7fa16`.

## Reproduce and remaining scope

The already registered dispatcher can run the temporary branch's reusable job:

```sh
gh workflow run io-uring-cpu-attribution.yml \
  --ref codex/io-uring-hotpath-optimization \
  -f mode=cost-ledger -f cost_stage=compare -f dispatch_budget=false
```

Record the newly dispatched SHA and host; do not mix absolute scores across
runners. Replay this specific downloaded artifact with:

```sh
python3 benchmarks/io_uring/cost_ledger/cost_ledger.py \
  --verify-output /path/to/io-uring-cost-45369498548-1 \
  --expected-candidate 7b7fa16e531ad38c125826f0c2d55c8a2deafcc9
```

The next evidence-led target is finer native request/poll/retirement attribution
and longer fixed-duration observation on a controlled native host. Allocation
and copy removal needs a separate ownership design and single-factor A/B; this
ledger is not permission to borrow payloads across admission/terminal boundaries.
No multishot, provided buffers, fixed files, SQPOLL or zero-copy mode is enabled.
No 24 h / 72 h endurance contract is replaced by these short tests.

Work remains on `codex/io-uring-hotpath-optimization`. Local and remote `main`
remain `e391c2f59e1cd0dc9d5cc8a1cbab215b058c488d`; this task makes no merge.
