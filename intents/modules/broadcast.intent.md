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
- Route input is the same `shared_ptr<const PlayerSession>` view exposed by
  `SessionManager`; Broadcast never requires mutable session ownership and can
  compose directly with authentication/lookup results.
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
  synchronously wait for another loop.
- Endpoint send may re-enter transport/core callbacks on the endpoint loop; no
  router/dispatcher lock is held.

## Backpressure Contract

- Hard fanout and byte budgets apply to every priority.
- Low-priority traffic is dropped at configurable soft fanout/byte limits.
- Duplicate endpoints, offline sessions, closed endpoints, hard limits, soft
  priority limits, invalid public plans, unavailable owners, and send rejection
  have distinct metric reasons.
- Dispatch tasks are bounded by endpoint count and aggregate payload bytes; a
  single payload above the per-task byte limit is rejected with its own reason.

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

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: management-loop routing, per-owner grouping, shared payload,
  bounded tasks, and explicit admission/delivery metrics.
- Deferred from the source design: group/AOI membership and higher-level game
  selection remain outside this router.
- Restored evidence: real TCP multi-loop reconnect, bounded high-fanout,
  ordering, slow-endpoint, and versioned latency/RSS benchmark coverage.
- Dropped behavior: none within the active router/dispatcher boundary.
