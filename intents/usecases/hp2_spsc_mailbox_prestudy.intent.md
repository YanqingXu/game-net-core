---
status: active
target: gamenet_hp2_spsc_mailbox_benchmark
migration_source: native
promote_gate: none
artifact_kind: benchmark
migration_mode: native
source_commit: none
source_paths: none
---

# Use-Case Intent: HP2 SPSC Mailbox And Owner Outbox Prestudy

Prestudy state: `KEEP-EXPERIMENTAL`.

## Intent

This concluded `EXPERIMENTAL-PRESTUDY` implementation slice evaluated one fixed-capacity
SPSC mailbox for each NetworkLoop→LogicShard and LogicShard→NetworkLoop edge,
using move-only HP1 `OwnedPacket` values and a coalesced source notification.

The current `GameCommandQueue`, Profile B implementation, EventLoop control/
lifecycle lanes, owner executor, installed APIs, public manifests, default
recipes, and production Core targets remain unchanged baselines.

## Topology And Memory Contract

- One mailbox has exactly one producer and one consumer for its lifetime.
- Capacity is a positive power of two. Storage for every slot is allocated and
  aligned once at construction; accepted push/drain performs no queue-node or
  payload allocation.
- Producer and consumer cursors occupy separate cache-line-aligned state. Each
  side owns its local cursor and publishes only the cursor observed by the
  other side with release/acquire ordering.
- `SpscMailboxMatrixPlan` checks `producerCount × consumerCount × capacity ×
  slotBytes` with overflow-safe arithmetic. A zero dimension, non-power-of-two
  capacity, or total above `maxMailboxBytes` is rejected; it never falls back
  to MPSC/MPMC or an unbounded container.
- `DataPlaneCommand<OwnedPacket>` owns route id, route generation, enqueue time,
  and exactly one moved packet. An owner-outbox result owns the same bounded
  value shape until the network owner consumes or cancels it.

## Admission And Notification Contract

- Producer admission returns exactly `Accepted`, `QueueFull`, `Stopped`, or
  `OwnerUnavailable`. Rejection retains no slot and leaves the caller's moved
  value in a valid but unspecified state only after an actual Accepted
  construction attempt; pre-admission lifecycle rejection does not consume it.
- Successful construction publishes the tail with release ordering. Consumer
  acquire observes a fully constructed value, invokes the owner visitor, then
  destroys the slot before publishing head.
- The source has one atomic notification-pending gate. Only the first
  empty→non-empty publication changes it clear→set and invokes the registered
  notifier; later pushes merge without another physical notification.
- A bounded owner drain keeps the gate set while backlog remains. When it sees
  empty, it clears the gate and immediately rechecks the acquire-visible tail;
  a producer racing that clear either observes the old set bit or sets it for a
  new notification. Accepted work cannot be stranded.
- A drain callback may request another owner turn but may not recursively drain
  the same source. Notifications never enter EventLoop's existing control,
  lifecycle, normal, or reserved functor queues in this prototype.

## Ownership, Generation, Re-entry, And Cross-Thread Rules

- Construction, drain, route validation, stop/finalization, cancellation, and
  destruction belong to the consumer owner. One named producer thread alone
  uses its copied producer handle.
- The source owns bounded mailbox state and notifier authority. A producer
  handle shares only finite generation/control storage and cannot mutate owner
  state directly or extend an EventLoop.
- Attach creates a nonzero generation. Detach first rejects admission, then
  invalidates the generation. Stale handles return `OwnerUnavailable` and
  cannot address a replacement source.
- The consumer visitor may re-enter higher-level route/stop behavior. No lock
  is held, but recursive drain of the same source is rejected.
- Network→logic and logic→network use distinct mailboxes and producer handles;
  no mailbox changes producer/consumer roles at runtime.
- HP1 `PacketView` never enters a mailbox. Cross-domain input first calls
  `retain()` and moves exactly one `OwnedPacket` into `DataPlaneCommand`.

## Lifecycle And Settlement

- `beginStop()` rejects new admission while already active producer calls and
  Accepted slots retain their obligations.
- Normal stop drains every Accepted command. Explicit cancellation is owner-
  only, visits and destroys every remaining slot, and increments a cancelled
  terminal count; silent clear is forbidden.
- Finalization requires no active producer call, zero queue depth, clear
  notification state, and `accepted == processed + cancelled`.
- Detach/destruction makes copied handles return `OwnerUnavailable`. Shared
  finite control storage may outlive the source only until copied handles are
  released; it owns no EventLoop or user callback after detach.
- Visitor exception is contained, converts the current Accepted item to an
  explicit cancelled terminal, begins stop, and leaves later items for bounded
  cancellation or drain. No exception crosses the producer thread.

## Route And Outbox Contract

- The logic consumer validates route id/generation before handler execution.
  An owner-outbox result captures that generation and the network consumer
  validates again immediately before the simulated send.
- Disconnect generation rollover makes old input/output terminally stale; it
  cannot reach a replacement route.
- Profile B topology is tested first. Profiles C/D remain later formal
  comparisons and no public Runtime factory or universal mailbox is added.

## Performance Hypothesis And Evidence Boundary

- payload copy is at most the one HP1 `retain()` copy;
- queue-node allocations per accepted message are zero;
- each direction emits at most one notification for one non-empty burst;
- compared with callback+mutex, callback+SPSC reduces allocation, lock, generic
  post, wakeup, handoff latency, and queue-age costs without weakening bounds or
  shutdown settlement.

The opt-in benchmark records both paths in development evidence. HP0 fixed-lab
and the formal 5%/3% promotion rule remain mandatory before any integration or
default switch.

The Windows development run showed zero candidate queue allocator calls,
locks, and generic posts plus a throughput signal, but its burst queue-age
guardrail was unstable and regressed in the median. The prototype is therefore
retained for later fixed-load replay as `KEEP-EXPERIMENTAL`; it is not eligible
for promotion and the single prestudy implementation slot is released.

## Verification

- `tests/contract/runtime_model/test_spsc_mailbox.cpp` covers capacity/memory
  rejection, move-only in-place construction, FIFO/wraparound, QueueFull,
  batch drain, producer/consumer ordering, stop/cancel accounting, exception
  containment, and zero residue.
- `tests/contract/event_loop/test_event_loop_mailbox_source.cpp` covers
  generation-safe handles, empty→non-empty coalescing, the clear/recheck lost-
  wakeup race, bounded continuation, recursive-drain rejection, detach,
  OwnerUnavailable, and Accepted terminal settlement.
- `tests/cmake/test_hot_path_benchmark_contract.py` enforces the single active
  prestudy, benchmark-only build graph, non-installation, exact contracts, and
  absence from public manifests/default targets.

## Non-Goals

- no production EventLoop mailbox source, Profile B switch, installed mailbox,
  MPSC/MPMC fallback, public Runtime API, stable send API, or HP2 integration;
- no HTTP/TLS/UDP/KCP/RPC/coroutine scope;
- no promotion conclusion before HP0 fixed-lab evidence.
