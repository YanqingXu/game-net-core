---
status: active
target: GameNet::core
migration_source: native
promote_gate: none
artifact_kind: installed-library
---

# Architecture Intent: Runtime Models

## 1. Intent

Runtime Models define supported ways to place TCP connection work and game
logic work without changing the owner-loop concurrency kernel. They turn the
existing EventLoop, TCP transport, bounded LogicLoop, and broadcast foundation
into explicit, testable deployment profiles.

Profiles are composition contracts, not new schedulers. They remain TCP-only
until the deferred transport and protocol intents are separately promoted.

## 2. Policies Are Independent

`ConnectionPlacementPolicy` chooses the EventLoop that owns a connection and
its transport callbacks. `LogicShardPolicy` chooses the LogicLoop or shard that
owns session/game-state mutation. They are independent decisions and must not
be encoded as one worker index.

Every cross-domain transfer carries immutable data or an explicit generation-
checked binding. It uses a bounded queue and returns Accepted, Saturated, or a
lifecycle rejection. No Profile gains an unbounded fallback queue.

## 3. Provisional Profiles

### Profile A: Network-only

The connection EventLoop owns TCP state and invokes application callbacks on
that owner thread. No LogicLoop handoff is required. This is the lowest-latency
and lowest-isolation profile and preserves the current Core default. RTM-R1
names its first runnable composition `SingleLoopInlineEvent`: TcpServer has zero
worker loops; framing and the immediate handler live in per-connection owner-
loop state; frame-count and frame-byte budgets bound one dispatch; one bounded
same-owner continuation carries any remainder. The handler must not block. A
configured wall-time overrun, continuation rejection, protocol fault, or output
overload is observable and terminal instead of switching thread or recursively
draining. This first composition is a non-installed example/support target, not
a stable public Profile API.

### Profile B: Network/logic split

An accept/base loop places each connection on a network EventLoop. Parsed
messages are handed to a separately selected LogicLoop through bounded
admission. Logic results return through the owning connection EventLoop. This
profile prioritizes clear ownership and isolation. RTM-R1 names the runnable
composition `MultiIoQueuedEvent`: TcpServer uses at least two worker EventLoops,
while one distinct caller-owned logic EventLoop drains the existing bounded
GameCommandQueue. The first accepted command while no drain is scheduled posts
one logic-owner drain; later producers merge behind that scheduled drain.
Each drain processes at most the configured command budget and queues one
continuation if backlog remains. Queue/admission rejection is typed and closes
the affected route; a logic wake/continuation rejection fail-stops Profile
admission and discards the bounded backlog rather than stranding Accepted work.
Every command and response carries one route generation. Logic output returns
only through the captured connection owner executor and revalidates generation
on that owner before send/close. This composition is non-installed until at
least two Profiles prove a common reusable API.

### Profile C: Hybrid

Connection callbacks stay on their network EventLoop, while selected stateful
or tick-driven work is sent to a bounded logic shard. Inline work is allowed
only for operations declared safe by the Profile contract; overload must not
silently switch execution domains. Hybrid is provisional until at least two
simpler Profiles have contract and performance evidence.

## 4. Ownership and Thread Affinity

- EventLoop owns connection registration, socket I/O, TcpConnection state, and
  transport callbacks.
- LogicLoop owns bound logic state and scheduled tick execution.
- A session/binding object records both placements and a generation; neither
  loop infers the other placement from its own index.
- Profile configuration is immutable after start unless a later intent defines
  migration with an explicit two-owner handoff protocol.
- Teardown revokes admission, drains or rejects already accepted work according
  to the Profile contract, unbinds the generation, then releases state on its
  owning thread.

## 5. Re-entry and Cross-Thread Operations

Network callbacks may re-enter owner-loop APIs, but never directly mutate
LogicLoop-owned state. Logic callbacks may enqueue a bounded response to the
connection owner; they cannot call transport state mutators directly. Close,
shutdown, or rebinding racing with queued logic work is resolved by binding
generation, so stale work is rejected rather than delivered to a replacement.

Tick scheduling is owned by LogicLoop. A saturated handoff or tick queue has an
observable result and a documented caller policy; spinning, recursive inline
execution, and silent loss are forbidden.

## 6. Profile Evidence

Each Profile must state and test:

- connection and logic owner threads;
- lifecycle admission and terminal shutdown result;
- handoff capacity, saturation outcome, and recovery;
- tick ordering and maximum batch/fairness budget;
- stale binding generation behavior;
- per-connection and per-shard memory budgets;
- callback re-entry and shutdown races;
- throughput, tail latency, memory, and shutdown comparison against the
  architecture baseline on Linux/epoll and Windows/IOCP.

## 7. Existing Verification Anchors

- `tests/contract/transport/test_tcp_transport_endpoint_dispatch.cpp` verifies
  owner-loop transport dispatch.
- `tests/contract/game_logic/test_logic_loop_contract.cpp` verifies bounded
  admission and explicit saturation.
- `tests/contract/game_logic/test_logic_loop_binding_generation.cpp` verifies
  stale binding rejection.
- `tests/contract/game_logic/test_logic_loop_lifecycle.cpp` verifies lifecycle
  convergence and accepted-work handling.
- `tests/integration/game_pipeline/test_game_server_pipeline_handoff.cpp`
  verifies the network-to-logic handoff path.
- `tests/integration/game_pipeline/test_game_server_pipeline_saturation.cpp`
  verifies overload behavior without an unbounded fallback.
- `tests/integration/game_pipeline/test_game_server_pipeline_shutdown.cpp`
  verifies multi-domain shutdown.
- `tests/integration/broadcast/test_broadcast_tcp_multi_loop.cpp` verifies real
  multi-loop connection placement and fanout.
- `tests/contract/runtime_model/test_network_only_profile.cpp` verifies the
  first runnable Profile A composition, bounded inline handler dispatch, zero
  cross-domain handoff, overload closure, and shutdown convergence.
- `tests/cmake/test_runtime_profile_contract.py` guards the non-installed
  Profile A topology and contract registration.
- `tests/contract/runtime_model/test_network_logic_split_profile.cpp` verifies
  Profile B multi-owner placement, bounded/coalesced handoff, generation-safe
  output, slow-consumer saturation/recovery, and two-domain shutdown.
- `tests/cmake/test_multi_io_queued_profile_contract.py` guards Profile B's
  non-installed topology, use of GameCommandQueue, bounded drain, typed failure
  handling, and owner-executor return path.
- `examples/runtime_profiles/multi_io_queued_echo.cpp` is the runnable Profile B
  TCP echo composition.

RTM-R1 adds Profile-specific contracts before exposing runtime Profile types.
Existing tests are the regression anchors, not a claim that all three Profiles
already exist.

## 8. Non-Goals

- no dynamic connection migration in RTM-R1;
- no work stealing across EventLoop owners;
- no protocol or experimental transport promotion;
- no hidden global executor or shared mutable game state;
- no Hybrid runtime implementation before the simpler Profile evidence exists.

## 9. Review Questions

- Are connection placement and logic sharding independently visible?
- Which thread releases every lifecycle-sensitive object?
- Can a callback re-enter or race teardown without bypassing generation checks?
- Does every cross-thread transfer have a bound and explicit result?
- Which Profile-specific contract proves lifecycle, saturation, tick, memory,
  and shutdown behavior?
