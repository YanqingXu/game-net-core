---
status: active
target: gamenet_multi_io_sharded_hybrid_echo
migration_source: native
promote_gate: none
artifact_kind: example
migration_mode: native
source_commit: none
source_paths: none
---

# Use Case Intent: Multi-I/O Sharded Hybrid Profile

## 1. Intent

RTM-R2 proves a TCP-only `MultiIoShardedHybrid` composition without changing
the installed owner-loop kernel. One base EventLoop accepts, at least two
TcpServer worker EventLoops own established connections, and at least two
distinct caller-owned logic EventLoops each own one bounded logical cell.

`ConnectionPlacementPolicy` selects only the network owner. `LogicShardPolicy`
independently maps an immutable player, room, or scene key to one logic cell.
An established connection never migrates when successive commands select
different cells. The support header/source, echo, contract, and benchmark are
not installed and do not authorize a stable Profile API.

## 2. Shard and Ordering Contract

The first policy is deterministic `StableHash` over key kind plus key bytes.
Configuration and shard count are immutable after start, so the same key maps
to the same cell for the Profile lifetime. Empty/oversized keys and invalid
policy results are typed route-local failures, not implicit shard zero.

Each cell owns one finite FIFO containing copied immutable payload, route
identity/generation, shard key, dispatch lane, and monotonically assigned cell
sequence. A cell processes commands strictly in cell-sequence order. Different
cells execute independently and make no global-order promise.

## 3. Hybrid Dispatch

`EventDriven` commands request one coalesced bounded owner drain.
`FixedTick` commands wait for that cell's TimerQueue fixed-rate cadence. A later
event command never overtakes an earlier fixed-tick command: an event drain
stops at a fixed-tick head, and the next tick releases the ordered prefix under
one total work budget. After the prefix exposes another event head, at most one
bounded continuation is posted. This explicit head-of-line tradeoff preserves
cell ownership/order while both dispatch lanes coexist.

No lane uses fixed delay, busy wait, recursive inline execution, control-source
admission, work stealing, or an unbounded fallback queue.

## 4. Ownership, Re-entry, and Return Path

- TcpServer-selected network owners exclusively own PacketFramer,
  TcpConnection, endpoint mutation, and route revocation;
- each logic EventLoop exclusively owns its cell handler, event drain, timer,
  cell sequence observation, and any external state bound by the caller;
- the router runs as bounded immutable classification on the network owner and
  receives no endpoint or mutable logic state;
- handlers receive immutable shard/tick context and no transport object;
- every reply/close posts through the captured connection owner executor and
  revalidates route generation on that owner;
- callbacks may re-enter stop; admission and generation are rechecked after
  router/handler return and before every side effect.

## 5. Backpressure and Lifecycle

Every cell has independent command, byte, payload, and key limits. Saturating
one cell closes only the affected route and cannot consume another cell's
capacity. Router exception/overrun, key rejection, handler exception/overrun,
output rejection, and endpoint overload are typed and route-local. A rejected
event-drain post or timer setup is cell-terminal and fail-stops Profile
admission because Accepted work may not be stranded.

`stopGracefully()` first revokes Profile admission and every route generation,
then closes/discards all bounded cell queues, begins TcpServer drain, and asks
each logic owner to retire its timer. The aggregate logic future remains
pending through committed event/tick callbacks and completes only when every
cell is retired. The caller keeps all logic EventLoops alive through Profile
destruction.

## 6. Metrics and Directional Evidence

The synchronized snapshot exposes network owner count; logic cell count;
event/fixed admission and handler counts; per-cell depth/byte high-water,
accepted/dropped work and maximum sequence; stable-hash selections; event
wake/merge/continuation and fixed-head deferral counts; fixed tick counts;
queue-age and network-to-logic tails; typed routing/queue/handler/output
failures; owner/order violations; exact cross-domain handoffs; and aggregate
shutdown discard/convergence.

The opt-in benchmark records per-cell balance, event/fixed throughput and tail,
queue high-water, working set, exact handoffs, and shutdown. Windows/IOCP and
Linux/epoll numbers are directional local evidence, not promotion evidence.

## 7. Verification

- `tests/contract/runtime_model/test_sharded_hybrid_profile.cpp` uses real TCP
  to prove independent connection and logic placement, two network owners, two
  logic cells, stable same-key placement, one connection targeting two cells
  without owner migration, cell FIFO/no-overtake, cross-cell progress,
  per-cell saturation isolation, generation-safe return, and aggregate stop.
- `tests/cmake/test_sharded_hybrid_profile_contract.py` rejects installed
  artifacts, coupled worker-index selection, missing per-cell bounds,
  fixed-delay cadence, direct logic-thread transport mutation, and unregistered
  contract/example/benchmark artifacts.
- `examples/runtime_profiles/multi_io_sharded_hybrid_echo.cpp` is the runnable
  TCP echo composition.
- `benchmarks/runtime_profiles/multi_io_sharded_hybrid.cpp` is the opt-in
  directional sharding/hybrid baseline.

## 8. Non-Goals

- no installed Runtime Profile, scheduler, actor, AOI, Room, World, script, or
  mutable game-state abstraction;
- no connection migration, work stealing, dynamic shard-count change,
  rebalance, cross-cell transaction, or global ordering;
- no UDP, KCP, TLS, HTTP, RPC, coroutine, or experimental transport work.
