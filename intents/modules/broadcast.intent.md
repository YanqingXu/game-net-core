---
status: active
target: GameNet::broadcast
migration_source: native
promote_gate: none
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
- Plans own shared payload bytes and endpoint references until dispatch tasks
  finish.
- `BroadcastDispatcher::dispatch` may be called on the management loop; it
  never calls `send` there. Every chunk is queued to its endpoint owner loop.
- Metrics for routing run on the router caller; scheduled-send metrics run on
  endpoint loops. Metric callbacks must therefore be thread-safe and must not
  synchronously wait for another loop.
- Endpoint send may re-enter transport/core callbacks on the endpoint loop; no
  router/dispatcher lock is held.

## Backpressure Contract

- Hard fanout and byte budgets apply to every priority.
- Low-priority traffic is dropped at configurable soft fanout/byte limits.
- Duplicate endpoints, offline sessions, closed endpoints, hard limits, soft
  priority limits, and send rejection have distinct metric reasons.
- Dispatch tasks are bounded by endpoint count and aggregate payload bytes; a
  single payload above the per-task byte limit is rejected with its own reason.

## Verification

`tests/contract/broadcast/test_broadcast_contract.cpp` verifies grouping,
deduplication, offline filtering, hard/soft budgets, payload sharing, bounded
task counts, explicit metric reasons, and execution of `send` on two distinct
endpoint owner loops.
