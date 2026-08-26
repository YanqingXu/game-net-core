# HP6 Adaptive Bounded Scheduler Prestudy

Date: 2026-08-25

Decision: HP6-A `DEFER`; HP6-B/C `SKIPPED-BY-EVIDENCE`

Evidence class: development-only synthetic cost, non-promotion

## Scope And Lifecycle

The default-off HP6-A planner composes per-phase count limits with estimated
time limits, backlog, oldest age and bounded weighted deficit for I/O, timer,
control, lifecycle and functor phases. One constructing owner owns only fixed
policy/deficit arrays and stop state; it owns no EventLoop work and invokes no
callback. Remainder requests a modeled later `poll(0)` round.

HP6-B pools and HP6-C hot/cold/NUMA layout were not prototyped because HP0 has
not identified a qualifying allocation, cache-miss or bytes-per-connection
hotspot.

## Correctness Evidence

The focused Windows MSVC Release contract passed 1/1 and its MSVC Debug
AddressSanitizer run passed 1/1. It covers invalid policies/observations, owner
affinity, count/time/deficit caps, one-item progress, age boost, weighted
saturated progress, minimum control/lifecycle service, zero-poll continuation,
stopped rejection and zero planner residue.

Six unchanged production regressions passed 6/6: EventLoop base, control
saturation, fair budget, lifecycle hub, wakeup coalescing and TimerQueue.

## Development Measurement

Ten sequential Release samples used 32,768 synthetic 100-us I/O items and 512
5-us items in each other phase. Both models completed 34,816 items, identical
3,287,040,000-ns simulated cost and equal checksums with zero residue.

| Metric | Fixed count | Adaptive time + deficit |
| --- | ---: | ---: |
| Maximum modeled round cost | 7,680,000 ns | 1,140,000 ns |
| Control first service | 6,720,000 ns | 580,000 ns |
| Lifecycle first service | 7,040,000 ns | 660,000 ns |
| Maximum modeled oldest age | 3,280,640,000 ns | 3,286,740,000 ns |
| Rounds | 512 | 6,554 |
| Planner elapsed median | 1,600 ns | 333,850 ns |

Maximum round cost fell 85.16%, while oldest age regressed 0.186% and planner
work/round count rose sharply.

## Missing Evidence And Decision

Estimated item costs do not establish real callback duration, backend poll
interaction, wakeup rate, queue age, Timer SLO, throughput, CPU or P99/P999.
There is no Profile A/B fixed-load replay, Linux run or clean HP0 ledger.

The mixed structural tradeoff is therefore `DEFER`. HP6-B/C are
`SKIPPED-BY-EVIDENCE`; production EventLoop, pools/layout, APIs and defaults
remain unchanged, formal HP6 stays `planned`, and the prestudy slot is released.
