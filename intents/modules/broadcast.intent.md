---
status: active
target: GameNet::broadcast
migration_source: mini_trantor
promote_gate: none
artifact_kind: installed-library
migration_mode: redesign
source_commit: 3eba368475a68f677aae920d4f299b155db23d57
source_paths: mini/net/broadcast/BroadcastRouter.h;mini/net/broadcast/BroadcastRouter.cc;mini/net/broadcast/BroadcastDispatcher.h;mini/net/broadcast/BroadcastDispatcher.cc
---

# Module Intent: Broadcast Router And Dispatcher

## Intent

The broadcast layer accepts an upper-layer-provided list of network sessions,
applies explicit fanout/byte budgets, groups endpoints by owner loop, and
schedules bounded send tasks. It does not compute AOI, rooms, teams, or world
state.

## Threading And Ownership

- `BroadcastRouter::route` runs on the SessionManager/management loop so session
  lifecycle state is read only by its owner.
- Route input is immutable `BroadcastTarget` value data copied on the
  management loop. Broadcast never observes or aliases a mutable
  `PlayerSession`; a target carries only endpoint delivery capability and an
  optional current-generation binding check.
- Plans own shared payload bytes and endpoint references until dispatch tasks
  finish.
- `BroadcastDispatcher::dispatch` may be called on the management loop; it
  never calls `send` there. Every chunk is queued to its endpoint owner loop.
- Plans carry non-owning `EventLoopExecutor` capabilities rather than raw loop
  pointers. Only `BroadcastRouter` can construct a plan, so callers cannot
  bypass global fanout/byte/dedup admission. Dispatch still revalidates endpoint
  state and rejects work when an owner stops accepting callbacks.
- Metrics for routing run on the router caller; scheduled-send metrics run on
  endpoint loops, with `Scheduled` emitted before the matching terminal
  `Sent`/`Dropped` event. Metric callbacks must therefore be thread-safe and must not
  synchronously wait for another loop. Metric exceptions are isolated and do
  not alter routing, delivery, progress, or outstanding reservations.
- Endpoint send may re-enter transport/core callbacks on the endpoint loop; no
  router/dispatcher lock is held. An endpoint exception is contained as
  `SendRejected` and does not leak an outstanding reservation.
- Dispatcher owner identities are registered once into an immutable,
  atomically published registry. The registration mutex is used only on the
  first observation of an owner; repeated task reservation, rollback,
  completion release, shutdown checks, and snapshots do not acquire it.
- Outstanding admission follows the fixed atomic hierarchy owner task ->
  owner logical bytes -> global logical bytes. A later-scope rejection rolls
  back every earlier scope synchronously in reverse order.
- Shutdown and the final reservation check share one atomic admission gate.
  A task is therefore ordered either before shutdown and retained normally, or
  after shutdown and fully rolled back as `OwnerShutdown`.

## Backpressure Contract

- Hard fanout and byte budgets apply to every priority.
- Low-priority traffic is dropped at configurable soft fanout/byte limits.
- Duplicate endpoints, offline sessions, closed endpoints, hard limits, soft
  priority limits, invalid public plans, unavailable owners, and send rejection
  have distinct metric reasons.
- Dispatch tasks are bounded by endpoint count and aggregate payload bytes; a
  single payload above the per-task byte limit is rejected with its own reason.
- Across consecutive plans, per-owner outstanding task/byte counts and a global
  outstanding logical-byte count are reserved before queue admission and
  released exactly once after task completion or admission rollback.
- The dispatcher exposes low-frequency global and per-owner atomic snapshots
  of current/peak tasks and bytes plus rejection counts. A multi-counter
  snapshot is diagnostic rather than transactionally consistent, but aggregate
  `tasks == 0` remains the convergence marker and is cleared only after that
  task's global-byte and owner scopes are released.
- `BroadcastDispatcher::shutdown()` is a thread-safe, idempotent admission
  close. Later chunks/plans are rejected with `OwnerShutdown`; tasks already
  admitted retain their shared payload and publish normal terminal progress.
- Low-priority plans are shed at explicit soft outstanding limits. Every
  QueueFull, Shutdown, OwnerUnavailable, EndpointClosed, EndpointOverloaded and
  policy rejection has a distinct reason.
- `DispatchSummary` reports routed/scheduled/accepted/dropped counts and
  per-reason counts. Its shared progress snapshot includes endpoint-loop
  terminal send results that occur after `dispatch()` returns.

## Verification

- `tests/contract/broadcast/test_broadcast_contract.cpp` verifies grouping,
  deduplication, offline filtering, hard/soft budgets, payload sharing, bounded
  task counts, direct use of read-only SessionManager-compatible session views,
  and execution of `send` on two distinct endpoint owner loops.
- `tests/contract/broadcast/test_broadcast_rejection_contract.cpp` verifies the
  complete rejection-reason matrix, public-plan validation, send rejection,
  owner expiry before dispatch, and endpoint close after task admission but
  before endpoint-loop execution.
- `tests/integration/broadcast/test_broadcast_tcp_multi_loop.cpp` verifies real
  `TcpTransportEndpoint` delivery across two distinct server worker loops and
  eight disconnect/reconnect cycles without retaining stale endpoints. It also
  holds one real client open without reading until the server reports output
  high-water backpressure under an explicitly small receive window, while a
  peer on the other owner loop receives an exact ordered multi-message stream
  without waiting for that slow client. Teardown waits for both server-side
  disconnect paths and base-loop connection removal, then stops the worker pool
  synchronously; it does not rely on a fixed post-stop sleep.
- `tests/contract/broadcast/test_broadcast_large_fanout.cpp` verifies bounded
  high-fanout dispatch and per-endpoint ordering with a slow endpoint present.
- `tests/contract/broadcast/test_broadcast_outstanding_budget.cpp` verifies
  per-owner/global reservations across multiple valid plans, exact rollback,
  low-priority shedding, QueueFull versus Shutdown/OwnerUnavailable mapping,
  endpoint overload/close reasons, concurrent no-overshoot at owner/global
  scopes, shutdown races, immutable owner-registry reuse, and final zero
  outstanding state.
- `gamenet_capacity_profile --scenario slow-broadcast-recovery` drives real
  TcpTransportEndpoint targets into bounded slow-reader output, samples
  dispatcher/TCP/retained-memory peaks, attributes every terminal rejection,
  and requires a stable low-water recovery window before teardown. Its
  observational process working-set peak must be no smaller than the explicit
  baseline, pressure, recovery, and post-teardown samples even when the
  independent periodic sampler misses a short-lived phase maximum.
- `gamenet_capacity_profile --scenario mixed-pressure-recovery` keeps the same
  real slow-reader Broadcast target set while a persistent bounded client pool
  paces short-lived healthy probes through that same TcpServer. Probe sockets
  use explicit nonblocking connect deadlines, send one fixed small payload,
  require its exact echo, close abortively, and are never admitted as Broadcast
  targets.
- Mixed-profile client workers own only distinct attempt slots and sockets.
  Server connection/message callbacks remain owner-loop-affine; benchmark
  coordination classifies post-publication connections as probes under one
  benchmark mutex and waits for cumulative server accept/close convergence
  before reusing the next batch.
- The mixed profile reserves explicit loop/server output headroom for at most
  one configured probe batch without changing slow connections' per-connection
  hard limit or Broadcast admission limits. Its v2 evidence requires exact
  attempt = success + typed failure, client-connect = server-accept =
  server-close, zero-failure healthy probes, a common paced interval, and
  internally consistent connect/probe P99 and attempt-rate measurements before
  the slow/Broadcast recovery result can pass.
- Candidate and dedicated mixed profiles request the same finite server
  `SO_SNDBUF` in each owner-loop established callback. This keeps the 32 KiB
  payload/eight-payload application limit while making typed overload
  independent of the host kernel's default send-buffer capacity.
- The scale-ready mixed profile hands every slow client socket to exactly one
  member of a fixed-size recovery-reader pool after the pressure sample. Each
  worker owns a stable disjoint socket shard, uses nonblocking bounded reads,
  and remains live through graceful server close; the driver does not touch
  those sockets again until all workers join.
- `gamenet.capacity_profile.v3` records the configured reader-worker ceiling
  and exact worker/assigned/closed socket counts. The mixed profile cannot pass
  unless every slow socket is assigned once, every assigned socket reaches a
  terminal client-side close, and the actual worker count equals the smaller
  of the configured ceiling and slow-client population. Historical v1
  slow-only and v2 mixed artifacts retain their original validation contracts.

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: management-loop routing, per-owner grouping, shared payload,
  bounded tasks, and explicit admission/delivery metrics.
- Deferred from the source design: group/AOI membership and higher-level game
  selection remain outside this router.
- Restored evidence: real TCP multi-loop reconnect, bounded high-fanout,
  ordering, slow-endpoint, and versioned latency/RSS benchmark coverage.
- Dropped behavior: none within the active router/dispatcher boundary.
