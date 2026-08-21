# ARCH-G1 Independent Architecture Review

Review date: 2026-08-22 (Asia/Shanghai)

Review scope:

- production owner-loop I/O Engine seam through IOE-R1/R2/C1;
- default-off, Linux-only, non-installed io_uring slices IOE-X1 through X9;
- the IOE-X10 fixed measurement contract, implementation, and commit-bound
  comparison evidence;
- Runtime Profile A/B/C/D Core-boundary impact;
- stable 0.3 source/layout compatibility obligations.

Base implementation checkpoint: `24ca10e4ec9a6358050d8aa10cec060a42925275`
(IOE-X9), with governance checkpoint
`a5ff7e6d823984a86e89146889f29f6615702ec3`. Second-pass fix-forward and IOE-X10
implementation checkpoint: `f5d39b800b4dd943531670aa09840c931c3dee4d`
(pushed to `origin/main`). X10 evidence:
`docs/development/benchmark_results/2026-08-22-ioe-x10-f5d39b8/evidence.json`,
generated from a clean clone of that exact commit.

Reviewer: independent Codex reviewer `/root/arch_g1_review`; not the IOE-X10
implementation author.

## Decision

`APPROVE`

This supersedes the first-pass `REQUEST-CHANGES`. The contract-first
fix-forward closes both original non-waivable blockers and two additional
second-pass destruction/rollback gaps:

- all four mutable Hub observations reject a foreign caller before reading
  owner state;
- Engine, Pump, Driver, and Hub destruction now enforce owner and fully stopped
  preconditions without a timed owner wait or fallback obligation release;
- a drained standalone Engine still rejects foreign-thread destruction;
- partial Pump construction rolls back its sources and empty Engine without
  turning an ordinary registration exception into live-Engine termination.

Approval is narrow: the present production seam and default-off,
source-private, non-installed io_uring experiment are architecturally sound at
the reviewed state. It does not approve a public backend selector, an installed
io_uring target or production replacement. X10 supports only the narrow
source-private continuation described in question 6.

## Review order and findings

### 1. Intent correctness

The active I/O Engine intent defines the right boundary: EventLoop is the
single-owner scheduler/event pump; Readiness retains registration identity and
Channel binding; Completion retains operation identity, generation, typed
result, storage lease, cancellation observation, and exactly-one terminal
retirement. IOE-X1-X9 remain Linux-only, default-off, source-private, bounded,
and non-installed. The X10 contract fixes the 256-route, 32-Accept, four by
64-route churn, 100 by 64-byte echo, warm-up, five-sample, interleaved protocol
without expanding runtime authority.

The fix-forward extends the active intent with explicit owner-only mutable Hub
observations, fail-fast fully-stopped destruction preconditions, and
transactional Pump-construction rollback. The reviewed implementation and
focused contracts now follow those requirements.

### 2. Public contract correctness

No Runtime Profile or io_uring header/target is installed. Production Linux
continues to use epoll; there is no public backend selector. A same-line
comparison against `api/baselines/v0.3.0-perf-r1-reviewed.json` reports no
stable header/target change.

The source-private APIs remain contracts. The affected Hub observations are no
longer `noexcept`; their declarations document owner-only access and their
implementations reject before the first mutable read. Destruction preconditions
and Pump initialization rollback are now stated in active intent and rules.
The stable installed surface remains unchanged.

### 3. Invariants

The reviewed normal paths establish the intended invariants:

- epoll registrations carry source plus nonzero generation; remove/re-register
  invalidates stale native and active-batch work;
- IOCP and io_uring completions retain operation identity/generation and do not
  become fake Channel read/write events;
- Hub operation-route and connection-slot generations prevent stale route
  delivery;
- one Recv and one Send are active per Hub route; send segment/byte and Hub
  aggregate capacity are finite;
- accepted fds have one RAII owner through listener factory re-entry and route
  transfer;
- graceful send drain, one write half-close, continued receive, first close
  reason, mailbox admission, and observer revocation are explicitly modeled.

The historical destructor fallback has been removed. A legal destructor now
observes already-published physical stop and zero residue; an illegal one fails
fast before source detach, ring close, notice clearing, slot clearing, or lease
release. The construction-only rollback is separate and is legal because no
operation can be admitted before both sources attach.

### 4. Thread-affinity correctness

#### T1 closed: mutable Hub observations reject before owner-state reads

Symbols and path:

- `IoUringTcpConnectionHub::{listening,listenerMetrics,phase,metrics}` are no
  longer declared `noexcept` and forward to the corresponding Impl methods;
- each Impl method calls `assertOwnerObservation()` before touching `listener_`,
  `lastListenerSummary_`, `phase_`, `metrics_`, `activeOperationRoutes_`, or
  `pendingSendBytes_`;
- the rejection predicate reads only EventLoop's owner identity and throws
  before any non-atomic Hub field is observed.

`testCapacityForeignMutationAndAggregateEventLoopQuit()` invokes all four APIs
from a real foreign thread, requires four typed rejections, then proves the
owner-side queries and owner-quit shutdown remain usable. Active intent,
thread-affinity rules, testing rules, header comments, implementation, dynamic
contract, and the static experimental guard now agree. No mutable Hub snapshot
is advertised as cross-thread-safe.

### 5. Ownership correctness

Normal ownership is explicit: EventLoop uniquely owns the production
dual-interface Engine object; readiness borrows Channel; Completion leases
retain only source-private operation/transport storage; Hub uniquely owns its
Pump, listener, transferred sockets, fixed route/operation tables, send bytes,
and stop promises; Adapter state owns admitted mailbox payload until transfer
or reconciliation.

The ownership proof now closes on destruction. Pump/Driver/Hub callers retain
their objects until the physical stop future is ready, while standalone Engine
callers must reach Shutdown and consume terminal notices. The destructor owns
only already-silent storage; it does not acquire convergence ownership. An
accepted-operation lease cannot be released by deadline or fallback cleanup.

Pump initialization separately retains rollback ownership until Channel and
lifecycle attachment both succeed. If either registration rejects, any earlier
source is removed and the operation-empty Engine reaches Shutdown with a zero
timeout before constructor unwind rethrows the original exception.

### 6. Lifecycle correctness

#### L1 closed: destruction requires prior physical stop and performs no drain

The revised paths are:

1. standalone `IoUringCompletionEngineImpl::~IoUringCompletionEngineImpl()`
   terminates unless it runs on the owner with `Shutdown`, physical quiescence,
   no ready notices, and zero Engine-owned bytes; only then does it close the
   empty ring;
2. `IoUringEventLoopPumpImpl::destroy()` terminates unless it runs on the owner
   after `Stopped`, a ready stop future, source detach, Engine quiescence, and
   notice retirement; it performs no shutdown or source removal;
3. Driver and Hub destructors likewise require owner execution, their published
   stopped future, and a physically stopped Pump. Hub additionally requires its
   maintenance source to be detached;
4. the historical 250 ms destructor shutdown, silent source-detach catch,
   notice/slot clearing, and owned-byte zeroing are absent.

The Pump subprocess contract accepts a real Recv with a retained lease and
destroys the Pump early. It must reach the terminate handler in under the old
250 ms wait interval; returning normally fails the child. The normal companion
then proves one cancellation terminal, lease retention through consumer entry,
stop-future publication, Channel/lifecycle detach, `DrainedAfterFailure` only
after terminal retirement, and zero operation/cancel/notice/owned-byte residue.

The standalone Engine subprocess reaches a clean `Shutdown` and then destroys
the Engine from a foreign thread; it must fail fast, proving owner affinity is
independent of residue. The construction-rollback contract puts EventLoop into
`Shutdown` so ring-fd Channel registration deterministically rejects, then
requires the original `logic_error`, zero lifecycle nodes, and clean empty-
Engine unwind. These close the second-pass owner and partial-construction gaps.

### 7. Implementation details

The production seam materially improves backend separation:

- EventLoop constructs one source-private dual-interface adapter and calls the
  `IoEngine` interface for wait, wakeup, readiness registration, completion
  notice pull, completion bookkeeping, quiescence, and progress;
- Linux production EventLoop owns `EpollReadinessPort` through the adapter;
  epoll tokens are generation values, not raw Channel pointers;
- Windows GQCSEx packets become distinct typed notices with direct consumers;
  `IocpPoller::poll()` is a rejecting compatibility shell and publishes no fake
  readiness;
- wait capacity and EventLoop dispatch/fairness budgets are separate.

The seam is not physically complete because stable 0.3 compatibility requires
dual inheritance/storage and because EventLoop still owns both Channel dispatch
and typed completion-consumer dispatch. These are reviewed debts, not reasons
to reintroduce backend-specific behavior into Core.

Hub plus Adapter now duplicate a substantial portion of production
`TcpConnection` mechanics: receive repost/pause, partial FIFO send, pending-byte
accounting, graceful half-close, close-reason selection, callback containment,
high/low/hard backpressure, notification scheduling, cross-thread admission,
and terminal publication. This duplication was useful for X3-X9 semantic
shaping, but further independent growth would create two TCP lifecycle
implementations. Do not copy more production behavior. Any authorized
continuation should remain source-private and use the
cross-backend contracts to identify a narrow shared semantic/controller seam
or a source-private I/O driver boundary; it must not merge Readiness and
Completion operation mechanics or modify stable `TcpConnection` in place.

### 8. Test completeness

Second-pass fresh execution on WSL2 Linux, GCC 13.3, Release, experimental
enabled:

- the X10 listener comparison target and all seven io_uring runtime contract
  targets built from scratch;
- 7/7 IOE-X1-X9 runtime contracts passed: standalone Engine, Pump, Driver, Hub,
  fixed-capacity Hub, listener, and Adapter;
- those tests include the four foreign-observation rejections, accepted-Recv
  early-destruction fail-fast, normal drain/lease/future/zero-residue companion,
  drained-Engine foreign destruction, and Pump-registration rollback cases;
- `tests/cmake/test_io_uring_completion_engine_contract.py` and
  `tests/cmake/test_migration_status_contract.py` passed;
- the stable 0.3 same-line comparison against
  `api/baselines/v0.3.0-perf-r1-reviewed.json` reports zero header fingerprint,
  header-set, target, or category change.

Independent X10 evidence verification:

- the manifest binds full commit
  `f5d39b800b4dd943531670aa09840c931c3dee4d`, Release, CPU affinity 0-1,
  GCC 13.3, Linux 6.18.33.2 WSL2, and the exact 256/32/256, four-by-64 churn,
  100-round-trip, 64-byte protocol;
- run order records one unretained warm-up per backend, then five formal
  samples per backend with alternating epoll/io_uring first position;
- all ten retained sample SHA-256 values recompute exactly; all ten raw samples
  independently pass `validate_ioe_x10_listener_comparison.py` against their
  declared backend;
- independently recomputed medians and io_uring/epoll ratios match the manifest
  exactly;
- every sample reports 516 connects, 512 admissions, 128,000 echoes, 512
  closes, four typed capacity rejections with recovery, route high-water 256,
  and zero common final residue;
- every io_uring sample reaches Accept/Recv/Send high-water 32/256/256 and
  Engine-operation high-water 544, records zero SQ rejection, and finishes
  with zero active Accept, operation, notice, pending byte, fd, and Engine-
  owned-byte residue. Epoll operation-only fields remain null rather than fake
  completion measurements.

The first pass also ran 12/12 broader focused tests covering the production
Engine adapter, Readiness/Completion split, EventLoop lifecycle, io_uring
slices, and combined Runtime Profiles. This independent pass does not claim a
new Windows/IOCP, sanitizer, repeat, or endurance run; those remain M1/promotion
evidence gates rather than waivers for this architecture approval.

## Required ARCH-G1 questions

### 1. Does the I/O Engine seam reduce concrete backend coupling?

Yes, materially but not completely. EventLoop no longer calls epoll/IOCP
implementations directly for the production wait path, Readiness and Completion
retain different typed identities, and IOCP no longer publishes fake Channel
events. Remaining coupling is concentrated in the private adapter and the
compatibility fields/forwarders listed below.

### 2. Which readiness/completion compatibility duties remain in EventLoop?

- public Channel registration methods and the retained/budgeted active-Channel
  batch, cursor, epoch, current-Channel retirement slot, and callback dispatch;
- direct typed completion notice pulling, observer source/pointer/generation
  validation, tie guarding, consumer invocation, exception containment, and
  wait-batch lease retirement under the historically named active-Channel
  budget;
- private IOCP socket-association, operation-retention, cancellation-obligation,
  quiescence, and metric forwarding;
- `maxIocpCompletionsPerPoll` compatibility mapping and
  `IocpCompletionPacketsDrained` metric vocabulary;
- byte-stable `unique_ptr<Poller>` storage recovered as the dual-interface
  Engine, plus `Poller`/Channel terminology in the stable header;
- for experimental io_uring only, the ring descriptor is still scheduled by a
  readiness Channel, while CQE identity/result remains in the Pump/Engine.

### 3. Which stable 0.3 bytes or ABI/source slots must remain until a breaking
line?

The project does not promise pre-1.0 binary ABI, but its 0.3 manifest enforces a
full stable-header zero diff. Therefore the following compatibility debt must
remain unchanged until an explicitly reviewed breaking line:

- Channel's three unreachable Windows pointer-sized members
  `iocpCompletionOperation_`, `iocpAcceptCompletionHead_`, and
  `iocpAcceptCompletionTail_`, together with the fingerprinted private
  declarations/friends that surround them;
- EventLoop's `std::unique_ptr<Poller> poller_` member type and corresponding
  private completion-forwarding declarations;
- the `Poller` virtual layout, including the inherited `poll()` slot used as a
  rejecting IOCP compatibility shell and the private completion/wakeup hook
  slots;
- the positional `EventLoopOptions::maxIocpCompletionsPerPoll` field;
- the existing numeric enum/aggregate positions in `EventLoopMetrics`, including
  `IocpCompletionPacketsDrained`; additions must remain append-only under the
  current source policy.

Physical cleanup of these fields/slots is not authorized by ARCH-G1 and must
not be mixed with io_uring promotion.

### 4. Do Runtime Profiles leak placement, tick, shard, or business policy into
Core?

No reviewed leak was found. Profile classes/options/metrics and
`ConnectionPlacementPolicy`/`LogicShardPolicy` composition remain under
non-installed `examples/runtime_profiles`. Core exposes only generic network
loop selection (`EventLoopSelectionPolicy`) and lower-level owner/executor,
timer, transport, and bounded-admission primitives. Player/room/scene keys,
fixed-tick contexts, cell queues, and business handlers do not appear in
`GameNet::core`. The existing common-capability disposition remains
`NO-PROMOTION`.

### 5. Do the io_uring Hub/Adapter copy too much production TcpConnection logic?

They have reached the acceptable limit for a source-private proof and would
copy too much if extended independently. X3-X9 needed enough duplicate
mechanics to compare completion semantics, but Hub/Adapter now cover nearly the
whole TCP mechanical state machine. That is not authorization to replace
production `TcpConnection` or to keep adding parallel semantics. Future work,
if evidence permits, must consolidate at a narrow driver/controller boundary
and retain backend-specific operation ownership rather than creating a second
public TcpConnection implementation.

### 6. With X10, continue source-private integration or pause the experiment?

Continue only source-private integration shaping: X10 is `PROMOTE` in that
narrow scope.

The reason is capacity and lifecycle validity, not overall performance parity.
All ten formal samples validate, capacity recovers, no SQ rejection occurs,
the fixed 32/256/256 Accept/Recv/Send concurrency is reached, and every sample
ends with zero residue. The median io_uring/epoll ratios are approximately:

- round trips/s and throughput: 0.499;
- P50: 2.233 and P99: 1.396, both worse for io_uring;
- P999: 0.765 and RSS high-water: 0.954, both directionally better;
- capacity recovery time: 3.768 ms versus 2.570 ms, worse for io_uring.

Those results do not support installing the target, exposing a selector,
replacing production epoll, or claiming performance superiority. They do show
a bounded, stable, zero-residue Completion topology with a useful P999 signal,
which is sufficient under the fixed X10 contract to continue source-private
composition and semantic shaping. The throughput/P50/P99 deficits must remain
explicit optimization evidence debt before any later installation or
production-equivalence decision. Future shaping must also avoid extending the
duplicate Hub/Adapter state machine identified in question 5.

## Re-review disposition and remaining M1 evidence gate

ARCH-G1 is `APPROVE`: T1, L1, Engine owner destruction, and Pump construction
rollback now have aligned intent/rules, implementation, negative contracts,
normal-drain companions, and fresh independent Release verification. The
stable 0.3 manifest remains unchanged.

The fixed X10 protocol is complete and independently validates `PROMOTE` only
for later source-private shaping. This architecture decision does not by itself
close M1: the documented Linux focused/repeat/sanitizer and default
Linux/Windows matrix plus governance-front unification remain required. Those
gates can restrict promotion or future experimental work; they do not
retroactively waive or weaken the owner/lifecycle conclusions in this review.
