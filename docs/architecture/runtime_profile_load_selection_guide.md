# Runtime Profile Load-Selection Guide

Status: official recipe/example guidance for the non-installed TCP Runtime
Profiles. This is not a public Runtime API or a capacity guarantee.

Choose from measured workload properties, not from a game-genre label. Record
at least concurrent connections, packet frequency and burst size, handler cost
distribution, authoritative tick requirements, handoff tail, broadcast fanout,
and the first backpressure budget expected to saturate. Re-run the matching
benchmark on the intended native platform before deployment.

## Quick Selection

| Profile | Connections and packet frequency | Logic cost | Tick requirement | Handoff | Broadcast | Backpressure fit |
| --- | --- | --- | --- | --- | --- | --- |
| A `SingleLoopInlineEvent` | One I/O owner; best when aggregate packet work fits one loop with clear utilization headroom, even if connection count itself is high | Only small, non-blocking, bounded handlers whose observed tail stays below the configured wall-time budget | None | Zero cross-domain handoff; lowest scheduling complexity | Suitable for owner-local replies and small owner-local fanout; no worker topology to spread network work | Framer/continuation and connection-output limits close the connection on overrun or saturation |
| B `MultiIoQueuedEvent` | Multiple network owners; suited to frequent or bursty packets needing prompt event-driven logic admission | Bounded logic that should be isolated from I/O, but one logic owner must still sustain total admitted work | No authoritative tick | Two explicit crossings per request/reply; watch N→L/L→N P99/P999 and queue age | Use installed broadcast grouping by network owner; do not route fanout through the single logic queue unless business ordering requires it | Shared command/byte limits and drain budget; route-local queue rejection, but wake/continuation rejection is Profile-terminal |
| C `MultiIoDedicatedFixedTick` | Multiple network owners; packets may wait until the next cadence and should arrive within a bounded per-tick batch | Simulation work whose batch cost plus output admission fits the tick budget | Required fixed-rate epoch with explicit skip-missed or bounded catch-up | Two crossings; queue-age floor and tail include cadence wait | Snapshot/prepare broadcast on the tick owner, then dispatch by network owner; include fanout cost in the tick budget | Queue limits plus `maxCommandsPerTick`; watch queue age, jitter, duration, skipped/catch-up ticks, and output pressure |
| D `MultiIoShardedHybrid` | Multiple network owners and independently selected logic cells; useful only when key-local parallelism is measured | Stateful work that can be partitioned by stable key without cross-cell transactions | Per-cell fixed-rate work must coexist with event work under the no-overtake rule | Two crossings plus shard selection; watch per-cell imbalance and queue age | Select recipients outside cell ownership and dispatch by network owner; sharded broadcast is not a cross-cell transaction | Independent per-cell limits isolate saturation, but fixed heads can defer later event work; timer/drain failure remains terminal |

Profile D's real-sharding evidence gate is satisfied by the private M3 gateway
Queued Event/Sharded Hybrid comparison and one-hour replay/fault run. It remains
provisional and non-installed because M5 concluded `NO-PROMOTION` for a common
Runtime surface.

## Measurement Rules

### Connections

Count established connections per network owner and measure owner-loop
utilization. Connection count alone does not select a Profile: idle connections
are cheap compared with frequent framing, handler, output, and broadcast work.
Choose A only while one owner's worst measured turn retains headroom. B/C/D can
spread network ownership, but their logic topology must be sized separately.

### Packet frequency and burst shape

- Prefer A for cheap, latency-sensitive messages when one loop absorbs the
  aggregate burst within its frame-count/byte budget.
- Prefer B when a message should enter logic promptly and producer wake
  coalescing handles bursts without making network callbacks execute logic.
- Prefer C when processing at the next authoritative tick is correct; a packet
  arriving just after a deadline naturally waits almost one interval.
- Consider D only when measured key distribution permits independent cells and
  event/fixed work needs cell-local ordering.

### Logic cost

Use P99/P999 handler duration, not the average. A handler in A must never block;
the wall-time check detects a contract violation but cannot preempt it. B
isolates logic from I/O but has one logic owner. C bounds a whole tick. D adds
parallel cells but a hot key remains single-owner and cannot be repaired by
work stealing.

### Tick and cadence

Select C only for an authoritative fixed-rate simulation. Pick skip-missed when
late replay would be harmful; pick bounded catch-up only when a finite replay is
required and measured to fit the next turns. D's fixed lane shares one ordered
cell FIFO with its event lane, so an earlier fixed head deliberately blocks a
later event until a tick. Installed `LogicLoop` fixed-delay behavior is not a
substitute for either contract.

### Handoff

For B/C/D, budget both immutable input transfer and generation-safe return to
the captured connection owner. Track network-to-logic, logic-to-network, queue
age, rejected owner posts, and stale-generation drops. If handoff tails dominate
otherwise cheap work and one owner has headroom, A may be the better recipe.

### Broadcast

Logic placement and delivery placement are independent. Produce immutable
recipient/payload values on their owning domain, then use `BroadcastRouter` and
`BroadcastDispatcher` to group bounded delivery by endpoint owner. Do not send
directly from a logic/tick/cell owner, and do not infer a logic cell from a
network-worker index.

### Backpressure

Configure and observe every layer: framing/input retention, command count,
queued bytes, payload bytes, per-drain or per-tick work, endpoint output
hysteresis/hard limit, broadcast outstanding tasks/bytes, and stop-time discard.
Selection is invalid if it depends on an unbounded fallback, recursive inline
drain, work stealing, or silent loss. Exercise saturation and fresh-route
recovery before comparing throughput.

## Evidence Anchors and Limits

The historical directional samples used four clients and 256-byte payloads:

| Profile | Recorded composition | Directional median highlights |
| --- | --- | --- |
| A | 4 connections, 10,000 messages each, one owner | Windows 122,760 RTT/s, P99 63.6 us, zero handoff |
| B | 4 clients, 2 network owners, 1 logic owner, 5,000 messages each | Windows 362,546 messages/s, N→L P99 96 us, L→N P99 64 us, queue-age max 162 us |
| C | 4 clients, 2 network owners, 1 tick owner, 2,000 messages each, 1 ms tick | Windows 127,956 messages/s, jitter/queue-age P999 1,024 us |
| D | 4 clients, 2 network owners, 2 logic cells, 2,000 messages each, 1 ms tick | Windows 120,857 messages/s, queue-age P999 2,048 us, cell imbalance 1.0 |

These samples were collected at different Profile checkpoints and are not a
paired ranking or deployment limit. The WSL/DrvFS results for C/D were much
lower than Windows and were explicitly classified as directional, not native
Linux regression evidence. Exact configurations, Windows/WSL figures, memory,
shutdown, and test gates are retained in
`docs/development/commit_bound_evidence_ledger.md`.

## Lifecycle Checklist

Before adopting a recipe, answer and test:

1. Which EventLoop owns accept, each connection, and each logic/tick/cell?
2. Who owns every queued value, callback gate, timer, endpoint, and stop promise,
   and on which owner is it released?
3. Which router/handler/output callbacks may re-enter stop or close, and what is
   revalidated after callback return?
4. Which typed bounded admission carries every cross-thread operation, and what
   obligation does `Accepted` create?
5. Which exact Profile contract, saturation case, cross-platform run, and
   shutdown test proves the selected configuration?

The Core anchors are the four tests under `tests/contract/runtime_model` and
`tests/integration/runtime_model/test_tcp_runtime_profiles.cpp`. The independent
consumer anchor is `docs/development/m3_gateway_closure_2026-08-23.md`.
