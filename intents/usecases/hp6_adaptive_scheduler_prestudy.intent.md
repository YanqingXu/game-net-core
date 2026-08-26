---
status: active
target: gamenet_hp6_adaptive_scheduler_benchmark
migration_source: native
promote_gate: none
artifact_kind: benchmark
migration_mode: native
source_commit: none
source_paths: none
---

# Use-Case Intent: HP6 Adaptive Bounded Scheduler Prestudy

Prestudy state: `DEFER`.

## Intent And Sub-Slice Decision

This concluded `EXPERIMENTAL-PRESTUDY` runtime slice evaluates HP6-A through a
pure owner-local planning model that composes the existing per-phase count
limits with estimated time budgets, backlog, oldest age and bounded weighted
deficit. It does not call EventLoop or callbacks and cannot replace current
fair-round scheduling.

HP6-B pool work and HP6-C TcpConnection hot/cold layout work are
`SKIPPED-BY-EVIDENCE`: HP0 has not identified Packet block, output segment,
mailbox allocation, cache miss or bytes-per-connection as a fixed-lab hotspot.
No pool, reclaim mailbox, field-layout or NUMA prototype is authorized here.

## Ownership, Re-entry, And Cross-Thread Rules

- one constructing EventLoop-style owner exclusively supplies observations,
  advances deficits, stops and destroys the planner. It owns fixed phase policy
  and deficit arrays, but no EventLoop queue, callback, timer, Channel or work;
- plan creation invokes no callback. A formal EventLoop integration would apply
  a returned quota, settle each phase's mandatory state, and only then permit
  callback re-entry; this prototype cannot recursively plan;
- foreign plan/stop calls return `WrongOwner` before reading mutable deficit or
  lifecycle state. Real cross-thread producers remain on existing bounded
  EventLoop admission and wakeup paths.

## Scheduling Contract

- phases are I/O, timer, control, lifecycle and functor. Every policy has a
  positive count limit, time budget, weight and maximum deficit;
- each round adds bounded weight credit. Backlog whose oldest age reaches the
  configured threshold receives a bounded age boost, never an unbounded drain;
- planned items are capped by backlog, count, deficit and estimated time. One
  first-item progress exemption is allowed when a nonzero item estimate exceeds
  the phase time budget;
- control and lifecycle receive at least one item whenever backlogged. Other
  phases also retain positive weight, so repeated saturated rounds make finite
  progress without recursive callback execution;
- any remainder sets `forceNonBlockingPoll`, modeling a following `poll(0)`
  round. Exhausted count/time/deficit reasons, backlog and oldest age remain
  observable;
- stop seals planning and discards no work because the planner owns only
  observations/quotas. Settlement requires no plan in flight and zero planner
  residue.

## Evidence Boundary And Verification

- `tests/contract/event_loop/test_adaptive_phase_scheduler.cpp` verifies option
  validation, owner affinity, count/time/deficit caps, minimum control/lifecycle
  service, age boost, weighted saturated progress, nonblocking continuation,
  stop and zero residue;
- the benchmark compares current fixed-count phase draining with the adaptive
  quota model on identical finite synthetic work, reporting completed work,
  simulated cost, max round cost, first control/lifecycle service latency,
  planner overhead, rounds, zero-poll continuations, checksum and residue;
- unchanged EventLoop fair-budget, control-saturation, lifecycle and timer
  contracts remain mandatory production regressions;
- synthetic estimated costs are development evidence only. No timing can close
  HP6 without real EventLoop Profile A/B fixed-lab P99/P999, backlog and oldest-
  age measurements.

## Non-Goals

- no production EventLoop option/order/metric/default/API edit, real callback,
  queue, pool, storage layout, memory allocator, NUMA, install/export, version
  or release;
- no promotion conclusion before HP0 and native fixed-load replay.

## Prestudy Decision

Windows MSVC Release and focused Debug AddressSanitizer contracts passed, as
did six unchanged EventLoop/timer regressions. Ten synthetic Release samples
completed identical work and simulated cost with equal checksums and zero
residue. Adaptive quotas reduced maximum modeled round cost from 7,680,000 ns
to 1,140,000 ns (85.16%), control first service from 6,720,000 ns to 580,000 ns
and lifecycle first service from 7,040,000 ns to 660,000 ns.

The tradeoff is material: rounds rose from 512 to 6,554, median planner overhead
rose from 1,600 ns to 333,850 ns, and maximum modeled oldest age regressed
0.186%. Because costs are estimates rather than real callbacks/polls and no
fixed-load P99/P999 evidence exists, HP6-A is `DEFER`, not integration.
HP6-B/C remain `SKIPPED-BY-EVIDENCE`; production EventLoop and storage stay
unchanged, formal HP6 remains planned, and the prestudy slot is released.
