# ADR: Owner-Loop I/O Engine and Runtime Models

- Status: Accepted for implementation
- Decision date: 2026-08-19
- Architecture gate: ARCH-G1
- Runtime checkpoint: `669ebb0a7c5c475dea74b12275c66a2ce1876804`
- First implementation slice: IOE-R1

## Context

The current Core has a strong owner-loop lifecycle model, but its backend
boundary is `Poller` plus several IOCP-specific escape hatches. epoll naturally
returns readiness. IOCP naturally returns the result of an identified submitted
operation. Today `IocpPoller` translates Accept/Read and Connect/Write
completions into `Channel::kReadEvent` and `Channel::kWriteEvent`, then stores the
operation pointer on Channel for the consumer to recover.

That translation made the first portable TCP migration possible, but it also
places completion identity, retention, and metrics inside a readiness-shaped
abstraction. Runtime composition has a similar implicit boundary: connection
placement and LogicLoop placement exist, but are not yet declared as independent
policies or supported Profiles.

## Decision

### EventLoop is the owner scheduler

One EventLoop owner thread uniquely owns one source-private I/O Engine. EventLoop
continues to own scheduling order, timers, control sources, pending functors,
fairness budgets, and lifecycle transitions. The Engine owns platform I/O
admission and delivery. No Engine method is callable by arbitrary threads;
cross-thread requests remain marshaled through EventLoop.

The shared Engine contract is deliberately small:

- bounded wait and delivery to the owning EventLoop;
- owner-thread registration/submission/cancellation observation;
- explicit accepted/rejected results;
- generation-safe readiness registration;
- lease-safe completion operation retirement;
- lifecycle convergence and backend-neutral progress counters.

Readiness and Completion remain separate native vocabularies behind that
contract. They do not inherit from a fake common event type.

### Readiness registration

Channel remains the callback binding for a readiness registration. A
registration has an identity and generation. Removal invalidates the generation
before the Channel can be destroyed; an old active-batch entry cannot dispatch a
same-address or same-fd replacement.

### Completion operation

A completion operation has an identity, kind, generation, retained storage
lease, result, cancellation state, and exactly one terminal retirement. A
successful submission owns a future kernel completion packet. Cancellation is
not consumption. IOE-C1 will deliver a typed completion notice directly to its
operation consumer and will remove synthetic Channel readiness from this path.

### Runtime Models

Runtime composition uses two independent decisions:

- `ConnectionPlacementPolicy`: selects the EventLoop that owns a connection;
- `LogicShardPolicy`: selects the LogicLoop that owns session/game-state work.

RTM-R1 will specify three TCP-only Profiles: Network-only, Network/logic split,
and provisional Hybrid. Every handoff is bounded and generation-checked. Hybrid
runtime code waits until at least two simpler Profiles have contract and
performance evidence.

## Ownership, Re-entry, and Shutdown

| Concern | Owner and rule |
| --- | --- |
| Engine | Unique ownership by EventLoop; created and released with the loop |
| Registration | Engine stores non-owning binding; higher layer removes it on the owner thread before destruction |
| Completion storage | Operation lease retained until its terminal packet is dequeued and retired |
| Connection | Its selected EventLoop owns transport state and callbacks |
| Logic state | Its selected LogicLoop/shard owns mutation and Tick execution |
| User callback re-entry | Allowed through documented owner APIs; the next dispatch item is revalidated |
| Cross-thread request | Marshaled by EventLoop or a bounded Profile handoff; never a direct Engine/state mutation |
| Shutdown | Revoke external admission, drain accepted work and completion obligations, invalidate bindings, then release on owner threads |

## Compatibility and Delivery Sequence

IOE-R1 adds a source-private Engine interface and a Poller adapter. The adapter
implements both internal interfaces: EventLoop keeps its byte-stable
`unique_ptr<Poller>` private storage declaration, while every production call in
`EventLoop.cc` recovers and uses the Engine seam. No stable-header fingerprint
or public API changes. This lets the adapter prove zero semantic drift before
backend extraction.

The compatibility adapter also preserves legacy post-`loop()` owner teardown:
an attached Acceptor may cancel its fixed AcceptEx pool during stack unwinding,
after final loop dispatch but before EventLoop destroys Poller. IOE-R1 closes the
backend before releasing retained storage exactly as Poller did; it does not
invent a blocking destructor drain. IOE-C1 must replace this escape with typed
terminal retirement owned by the Completion Engine.

IOE-R2 introduces the native Readiness Engine and keeps Channel on that path.
IOE-C1 introduces typed IOCP completion notices, moves completion retention and
association behind the Completion Engine, then deletes Channel's IOCP operation
mailboxes and EventLoop's backend downcasts. Public API changes, if any, require
a later additive review after both native engines have evidence.

## Current Coupling Inventory

The locations below are the implementation checklist, not merely examples.
Line numbers refer to the ARCH-G1 runtime checkpoint and may move as slices land.

| Coupling | Current location | Planned owner |
| --- | --- | --- |
| EventLoop constructs default Poller | `src/core/net/EventLoop.cc:338` | IOE-R1 Engine factory/adapter |
| Normal and final-drain waits call Poller directly | `src/core/net/EventLoop.cc:395`, `:419`, `:462` | Engine wait/dispatch seam |
| Shutdown asks Poller for pending completions | `src/core/net/EventLoop.cc:440`, `:458`, `:480` | Completion obligation query |
| Socket association, operation retention/tracking are Poller virtual hooks | `include/gamenet/core/net/Poller.h:44` | Completion Engine |
| EventLoop exposes internal IOCP retention forwarding | `include/gamenet/core/net/EventLoop.h:242` and `src/core/net/EventLoop.cc:957` | source-private Engine access |
| EventLoop downcasts to IocpPoller for association cleanup and metrics | `src/core/net/EventLoop.cc:967`, `:1106` | typed Engine capability/progress |
| Public options contain IOCP batch width | `include/gamenet/core/net/EventLoop.h:124` | compatibility option mapped by adapter; later capability review |
| Metric vocabulary contains IOCP packets | `src/core/net/EventLoop.cc:1115` | backend event plus neutral wait/dispatch metrics |
| Channel installed header stores IOCP operation mailboxes | `include/gamenet/core/net/Channel.h:82`, `:100` | Completion Engine/operation consumer in IOE-C1 |
| IocpPoller maps operation kinds to fake read/write readiness | `src/core/net/poller/IocpPoller.cc:40`, `:250`, `:257` | typed Completion notice |
| Acceptor recovers completed accept operations from Channel | `src/core/net/Acceptor.cc:355`, `:762` | accept completion consumer |
| Connector/Acceptor/TCP transport call EventLoop completion hooks | `src/core/net/Connector.cc:285`, `src/core/net/Acceptor.cc:653`, `src/core/net/platform/IocpTcpTransport_win.cc:458` | Completion Engine submission/lease API |
| TcpClient preserves IOCP socket association through EventLoop | `src/core/net/TcpClient.cc:766` | Completion Engine association lifetime |

## Concrete Test Map

Each path in the “new contract” column is the required test entry for that
slice. It will be added with the slice rather than as an empty placeholder.

| Slice | New contract | Regression anchors | Required failure fixture |
| --- | --- | --- | --- |
| IOE-R1 | `tests/contract/io_engine/test_io_engine_poller_adapter.cpp` | Poller, EventLoop, fair-budget, Channel active-batch tests | adapter rejects non-owner mutation and preserves shutdown drain |
| IOE-R2 | `tests/contract/io_engine/test_readiness_engine.cpp` | Poller and Channel lifetime tests | stale registration generation after remove/re-register |
| IOE-C1 | `tests/contract/io_engine/test_completion_engine.cpp` | IOCP sync-error, partial/segmented write and completion-drain tests | cancel-before-dequeue retains lease and emits one terminal notice |
| Network-only | `tests/contract/runtime_model/test_network_only_profile.cpp` | TcpServer/TcpConnection lifecycle and memory tests | callback/thread mismatch and bounded shutdown |
| Network/logic split | `tests/contract/runtime_model/test_network_logic_split_profile.cpp` | LogicLoop and pipeline handoff/saturation/shutdown tests | saturated handoff, stale binding, close race |
| Hybrid contract | `tests/contract/runtime_model/test_hybrid_profile_contract.cpp` | broadcast multi-loop and pipeline Tick tests | forbidden inline fallback and Tick starvation |
| Cross-platform profiles | `tests/integration/runtime_model/test_tcp_runtime_profiles.cpp` | pipeline and broadcast integration tests | identical lifecycle result on epoll and IOCP |

For every Profile, the focused contracts cover lifecycle, saturation, handoff,
Tick/fairness, memory budget, and shutdown. The core and Phase 4 benchmarks add
throughput, tail latency, and memory comparison.

## Numerical Guardrail

[The ARCH-G1 baseline](../development/io_engine_baseline.md) is the comparison
point for IOE-R1. A single local run is directional, so it is not a statistical
release threshold. IOE-R1 closes only when both platforms preserve contracts and
show no unexplained material regression. A change beyond 5% in echo throughput
or P99, connection memory, or shutdown time triggers a repeated paired sample
and a written accept/fix decision.

## Rejected Alternatives

- Making EventLoop an epoll-shaped base class: it leaks readiness assumptions
  into Completion and scheduling.
- Treating every completion as synthetic read/write readiness: it loses
  operation identity and makes cancellation/retention harder to prove.
- Publishing a backend abstraction in installed headers during IOE-R1: the
  common capability surface is not yet supported by two native implementations.
- Coupling connection and logic placement by worker index: it prevents explicit
  isolation, sharding, and independent capacity decisions.
- Building Hybrid first: it maximizes ambiguous execution paths before simpler
  lifecycle and overload contracts are measured.

## Consequences

The near-term cost is an internal adapter layer and temporary compatibility
forwarders. The benefit is that subsequent extraction has one bounded migration
surface and concrete failure tests. The ADR leaves no open abstraction-boundary
decision for IOE-R1: implementation starts with the source-private adapter, not
with another architecture round.
