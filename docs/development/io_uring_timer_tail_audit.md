# Native timer-tail diagnosis and precise-wait audit

This is bounded development evidence on `codex/io-uring-hotpath-optimization`,
not a promotion or production-latency guarantee. No merge to `main`, change of
default backend, or 24 h / 72 h endurance claim is made.

## Reproduce first, then change the runtime

The initial successful [diagnosis run 45351361451](https://github.com/YanqingXu/game-net-core/actions/runs/45351361451)
used observer `e58d765f5d29f211d0dd45564072b9088c87c3c8` and unchanged runtime
`66a106fe3c972d3642fcc15857b9daaab728e265`. It reproduced a staircase of timer
lateness without network I/O or foreign-thread wakeups. A 100 us timer was
typically about 962 us late; a 900 us timer about 162 us late. Poll entry/exit
records located nearly all of that delay inside a requested 1 ms wait.
Paced network traffic reproduced the same pattern on epoll and io_uring.

Only after that run completed was runtime fix
`2ab07c2d597aa89f9c258604dd1888cb88008e25` committed and tested in successful
[comparison run 45353943571](https://github.com/YanqingXu/game-net-core/actions/runs/45353943571).
The comparison rebuilt the unchanged baseline and the fix on the **same**
GitHub-hosted native Ubuntu 24.04.5 machine: Xeon 6975P-C, kernel
`7.0.0-1014-aws`, GCC 13.3, CMake 3.31.6, owner/peer pinned to CPUs 0/1 on
different physical cores. The earlier diagnosis used a different host; its
absolute scores are not used as the before arm of this comparison.

The experiment uses five rotated blocks, identical installed-package consumers,
Release flags, 64 connections, 4 KiB frames, warmup and fixed work. Saturated
and no-timer controls use 4,096 rounds; paced traffic uses 1,024 rounds with
1 ms between rounds. There are 100 uninstrumented scored trials, 12 separate
observer traces, and 10 additional kernel-scheduler traces. Fixed work is not
equal observation duration; all timer counts and exposure durations are kept.

## Root cause and bounded fix

TimerQueue rounds a positive timeout up to integer milliseconds for legacy
waits. EventLoop then used that rounded timeout even when the next absolute
deadline was less than 1 ms away. Ready I/O often hides this; idle and paced
traffic expose it. The Linux API distinction is documented in
[epoll_wait(2)](https://man7.org/linux/man-pages/man2/epoll_wait.2.html):
`epoll_pwait2` accepts a timespec, but neither API promises real-time scheduling.

The fix passes the earliest absolute deadline through the source-private
IoEngine/ReadinessPort path and recomputes the remaining nanoseconds immediately
before `epoll_pwait2`. The existing 10 s maximum wait, zero-time polling for
pending work and all dispatch phases remain in effect. EventLoop uses its
existing TimerQueue friendship only to read the earliest key; timer mutation,
expiration, cancellation and callback ownership stay in TimerQueue.

Compatibility and error behavior are explicit:

- CMake probes declaration/link availability. Builds without the libc symbol
  retain the ceil-millisecond `epoll_wait` path.
- Kernel `ENOSYS` is cached per port and falls back, recomputing the remaining
  absolute deadline after the failed probe. It does not restart an old delay.
- Expired deadlines poll with zero timeout. `EINTR` returns to the owner loop
  without retrying the original wait. Other errors remain fail-fast.
- No timerfd, timer busy-spin, new thread, public API/header change, callback
  domain or ownership transfer was introduced. Windows retains its old path.

The owner thread remains the sole mutator. Running / Quiescing / FinalDraining /
Shutdown transitions, target-terminal-plus-notice-consumption lease retirement,
accepted-byte drain before half-close and physical stop completion are unchanged.
The I/O, timer, control, lifecycle, functor phase order is unchanged, as are the
64 / 1,024 / 64 / 1,024 / 1,024 work budgets and io_uring notice budget 16.
The two installed experimental io_uring archives are byte-identical; only Core
changes. Defensive review specifically checked re-entry, cancellation and
retirement boundaries, rather than treating faster timing as correctness proof.

## Same-host results

All P99 figures below are the median of five **per-trial** P99 values, not a
pooled percentile. They come only from uninstrumented executables.

| Timer scenario | Before P99, us | After P99, us |
| --- | ---: | ---: |
| Idle phase scan | 964.462 | 65.640 |
| Idle absolute 1 ms cadence | 1,049.755 | 64.498 |
| Paced epoll | 1,058.153 | 107.891 |
| Paced io_uring | 1,058.171 | 122.130 |
| Saturated epoll | 85.969 | 98.518 |
| Saturated io_uring | 216.617 | 198.607 |

The same-host phase scan makes the mechanism particularly visible:

| Requested one-shot delay, us | Before median lateness, us | After median lateness, us |
| --- | ---: | ---: |
| 100 | 963.200 | 62.859 |
| 250 | 813.201 | 62.904 |
| 500 | 563.274 | 62.793 |
| 750 | 313.267 | 62.928 |
| 900 | 163.216 | 62.902 |

The staircase disappears; idle-cadence P99 falls 93.9% and paced io_uring P99
88.5%. This is a fix for the reproducible quantization cause, **not** every tail:

| Across all five trials | Before callbacks / over 1 ms | After callbacks / over 1 ms | After worst, us |
| --- | ---: | ---: | ---: |
| Idle phase scan | 10,000 / 2 | 10,000 / 2 | 2,445.915 |
| Idle cadence | 15,000 / 936 | 15,000 / 3 | 1,899.482 |
| Paced epoll | 6,730 / 400 | 7,131 / 0 | 176.894 |
| Paced io_uring | 7,560 / 334 | 7,906 / 1 | 2,141.043 |

Saturated epoll's P99 increased by 12.549 us in this small sample, and its
largest callback was 884.096 us after versus 176.495 us before. Saturated
io_uring's worst was 233.610 us after versus 348.794 us before. Neither arm
missed a saturated 1 ms tick. These observations do not establish a universal
improvement or a stable maximum; untraced outliers cannot be assigned causes
from a different traced execution.

No-timer saturated controls show no throughput benefit from this timer fix:

| Median of five | epoll before | epoll after | io_uring before | io_uring after |
| --- | ---: | ---: | ---: | ---: |
| Echo throughput, MiB/s | 1,647.975 | 1,640.637 | 1,032.309 | 1,027.550 |
| Owner CPU, ms | 1,202.380 | 1,205.550 | 1,981.903 | 1,993.009 |
| Round-trip P99, us | 193.881 | 193.652 | 284.236 | 286.867 |

Throughput medians changed -0.45% / -0.46%, owner CPU +0.26% / +0.56%.
Five short blocks are not an equivalence proof. io_uring remains 37.4% below
epoll throughput and uses 65.3% more owner CPU on this host/workload.

Idle-cadence owner CPU is 13.982 ms before and 13.991 ms after per 3,000
callbacks. Separate idle traces contain 3,001 polls in both arms: the fix is
not a sub-millisecond zero-time polling loop. Phase-scan CPU rises from 9.269
to 10.549 ms per 2,000 callbacks while elapsed time falls from 2,125.318 to
1,120.272 ms. Paced io_uring owner CPU rises 2.73% (504.082 to 517.831 ms),
with more timer callbacks delivered instead of missed ticks. These are costs
to retain, not evidence of a free optimization.

Slow-close io_uring stop medians are 254.614 / 254.735 ms with the same 250 ms
peer hold and 8 MiB pending at stop. All accepted frames are delivered, peers
reach EOF, terminal accounting closes and final native residue is zero.

## Scheduler and owner-work decomposition

All ten separate scheduler captures passed replay, with matched
`sched_switch`/`sched_wakeup` events and no lost-event indication. Clock and
owner identity are checked. The following are individual worst callbacks in
those **instrumented** executions, not benchmark scores:

| Trace / arm | Lag, us | Blocked, us | Runnable but off CPU, us | Scheduled, us |
| --- | ---: | ---: | ---: | ---: |
| Paced io_uring before | 1,076.488 | 1,055.373 | 3.422 | 17.693 |
| Idle cadence after | 1,016.722 | 23.530 | 987.287 | 5.905 |
| Saturated io_uring before | 1,501.365 | 0 | 1,376.727 | 124.638 |
| Saturated epoll after | 158.118 | 0 | 0 | 158.118 |
| Saturated io_uring after | 229.841 | 0 | 0 | 229.841 |
| Paced epoll after | 1,184.142 | 55.343 | 712.456 | 416.343 |

Blocked + runnable + scheduled partitions the lag without overlap. Scheduled
time can include IRQ/steal time and is not exactly thread CPU. Poll and message
spans are a different, overlapping view and must not be added to this sum.
In the saturated after traces, epoll's 158.118 us contains 122.219 us in message
callbacks; io_uring's 229.841 us contains 49.233 us in message callbacks and
2.883 us in poll calls. Its remaining 177.725 us is owner-side driver/Pump/
phase work plus instrumentation/other unclassified work, not proven scheduler
delay. A finer native-operation/Adapter breakdown remains future work.

The paced-before example supports the blocked rounded-wait cause. The idle-after
and saturated-before examples separately prove that runnable scheduling delay
can create millisecond spikes. They do not prove that every untraced maximum
has the same cause. Observer overhead is isolated from scores, and sequential
wall/thread-CPU clock skew is reported rather than converted into false
negative off-CPU time.

## Validation and evidence integrity

- [CI 45353948365](https://github.com/YanqingXu/game-net-core/actions/runs/45353948365)
  passed all six producers plus aggregation: Linux Debug, Release, ASan/UBSan,
  TSan and Windows MSVC Debug/Release. The five full suites execute 133 tests
  each; TSan executes its 105-test threading selection. Both Linux experimental
  contract suites pass all 11 targets; ASan includes the 1,000-unit fuzz smoke.
  Installed, extracted and upgrade consumers pass. Downloaded six-producer
  evidence was independently replayed with `verify_ci_evidence_set.py` and
  matched the expected run/attempt/SHA, inventories and file hashes.
- Native comparison passed all 11 Release io_uring contracts for each installed
  arm, raw operation/frame/EOF/retirement checks, and independent trace replay.
- WSL development-only validation passed 24 relevant Core/io_uring contracts
  with the precise path and again with the libc feature forced off. Readiness
  deadline/error-policy, TimerQueue and fairness tests each passed 100 repeats.
  WSL measurements are not used in any performance table.
- Nine timer-evidence tests and the existing 30 acceptance-evidence tests pass.
  The audit follow-up adds strict scheduler inventory, affinity and budget
  rechecks; it rejects missing/duplicate/wrong-arm traces. It changes only the
  verifier/tests/docs and was replayed against the retained `2ab07c2` artifact.
- The extra [Core benchmark run 45353941243](https://github.com/YanqingXu/game-net-core/actions/runs/45353941243)
  passed its native Linux performance and capacity budgets. Its Windows job is
  hard-coded to self-hosted labels, while the repository Runner API reported
  zero registered runners. The unstarted queue was cancelled; paired aggregation
  correctly failed closed (`linux=success`, `windows=cancelled`). This run is
  **not** a green Windows or complete paired-performance gate. No runner policy,
  baseline or budget was weakened. This does not invalidate the separate fully
  successful six-job hosted CI or the native Ubuntu comparison.

The main native artifact is
[io-uring-timer-compare-45353943571-1](https://github.com/YanqingXu/game-net-core/actions/runs/45353943571/artifacts/14902306344),
239,988,988 ZIP bytes. Its SHA-256 was checked against GitHub's authoritative
artifact digest before extraction:
`461902e64bfaba45893a5928ff909a7f9de1a854be6cbce6792f494a7ebb239f`.
Manifest SHA-256:
`e27b8380fb1ec803322d4eeb18d2a0b60f17aae6e27526c769bef3719c5488ea`.
The verifier replays raw results and every manifest file hash, verifies the
identical compiler settings and experimental archive, and rejects any runtime
delta outside the eight precise-wait files. The 203 baseline and 212 candidate
source blobs were additionally compared with independently known Git trees,
not merely with self-declared artifact revisions.

The initial diagnosis artifact
[14900797232](https://github.com/YanqingXu/game-net-core/actions/runs/45351361451/artifacts/14900797232)
has ZIP SHA-256
`b4fc5c6157d93cfe4e0cd404c748f987a0ec1a6aeb5a4cf6f72a06699afa454a`
and manifest SHA-256
`cf25cd9b1cfa1d864f345889875b3455ba033f2b9228cd6245c1148ad7e4a003`.
Its baseline/observer source blobs also match the authoritative Git revisions.
The v1 evidence is replayed with its archived v1 verifier, not reinterpreted as
v2. Both archives and extracted data are retained in the task workspace;
Actions retention is 30 days.

## Re-run and remaining scope

To run the same controlled comparison on a new native hosted Ubuntu job:

```sh
gh workflow run io-uring-cpu-attribution.yml \
  --ref codex/io-uring-hotpath-optimization \
  -f mode=timer-tail -f timer_stage=compare -f dispatch_budget=false
```

Record that new run's actual SHA and host; never combine its absolute scores
with this host. To replay the retained comparison after downloading/extracting:

```sh
python3 benchmarks/io_uring/timer_tail/timer_evidence.py \
  --verify-output /path/to/io-uring-timer-compare-45353943571-1 \
  --expected-candidate 2ab07c2d597aa89f9c258604dd1888cb88008e25
```

`benchmarks/io_uring/timer_tail/README.md` describes the minimal idle and network
reproducers. The current audit commit preserves tested runtime, C++ tests and
consumer/observer binaries; the native scores and six-job CI belong specifically
to `2ab07c2`, not to a later documentation/verifier commit.

Remaining work is longer fixed-duration observation on a controlled native host,
finer breakdown of the busy owner-side io_uring work, and broader native
capacity/endurance evidence. The experiment does not solve io_uring's epoll
throughput deficit, establish production readiness, or justify raising budgets.
`main` remains `e391c2f59e1cd0dc9d5cc8a1cbab215b058c488d`; no merge is authorized
by this audit.
