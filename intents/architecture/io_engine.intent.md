---
status: active
target: GameNet::core
migration_source: native
promote_gate: none
artifact_kind: installed-library
---

# Architecture Intent: Owner-Loop I/O Engine

## 1. Intent

The I/O Engine boundary separates EventLoop scheduling from platform I/O
semantics without pretending that readiness and completion are the same model.
EventLoop remains the owner-thread scheduler and event pump. A source-private
Engine owns backend registration, submission, wait, cancellation observation,
and delivery of typed backend events to that EventLoop.

The first slice is an internal seam over the existing Poller implementation.
It must preserve public API, callback order, lifecycle, and performance before
the readiness and completion implementations diverge behind that seam.

## 2. Semantic Boundary

- Readiness reports that a generation-safe registration may make progress.
- Completion reports the terminal result of one identified operation.
- EventLoop schedules both, but it is not itself an epoll Reactor or an IOCP
  completion implementation.
- Channel is the readiness registration and callback binding. It is not the
  long-term carrier for completion-operation identity.
- Completion operation kind, identity, generation, storage lease, result,
  cancellation observation, and terminal retirement stay explicit.
- The shared abstraction is ownership, admission, bounded waiting, dispatch,
  and shutdown convergence; it is not a fake read/write event vocabulary.

## 3. Owner Thread and Cross-Thread Rules

One EventLoop owner thread owns one Engine instance. Only that thread may:

- register, modify, or remove readiness registrations;
- submit backend operations after loop-level admission;
- dispatch returned readiness or completion notices;
- retire registration generations or operation leases;
- advance the Engine lifecycle state.

External threads may only use existing EventLoop admission paths. They enqueue
work with `runInLoop`, `queueInLoop`, or a registered control source and wake
the loop. They never call the Engine directly. The seam therefore introduces
no second synchronization domain.

## 4. Ownership and Lifetime

- EventLoop uniquely owns and releases its Engine.
- A readiness registration observes a Channel owned by its higher-level
  subsystem. Removal invalidates the current generation before destruction.
- A successfully submitted completion operation retains its storage lease
  until the kernel packet is dequeued and terminally retired.
- Cancellation requests progress toward retirement; they do not consume the
  completion or authorize early storage release.
- Backend socket association and operation retention are Engine concerns, not
  application callback concerns.
- Shutdown reaches `Shutdown` only after accepted control work, registrations,
  and completion obligations have converged according to their model.

## 5. Re-entry and Dispatch

User callbacks can re-enter EventLoop APIs and can remove another registration,
close a connection, queue more work, or request quit. Engine wait never invokes
user code. EventLoop dispatches a bounded snapshot and revalidates registration
generation or completion identity before each callback. The current-dispatch
retirement slot continues to protect a Channel released by its own callback.

Wait and dispatch have separate budgets. A busy I/O source cannot indefinitely
starve timers, control sources, or pending functors, and a busy task queue cannot
prevent the loop from observing completion retirement.

## 6. Lifecycle and Failure Results

The Engine follows `Running -> Quiescing -> FinalDraining -> Shutdown` with the
owning EventLoop. The source-private Engine state represents both EventLoop
Quiescing and FinalDraining as `IoEnginePhase::Quiescing`; EventLoop remains the
authority for the finer scheduler phase.

- Running admits normal registrations and operations.
- Quiescing rejects new external work while still observing accepted work.
- FinalDraining uses bounded nonblocking waits until registrations and
  completion obligations are retired.
- Shutdown owns no live backend registration or retained completion lease.

Submission and registration failures remain explicit and synchronous when no
kernel obligation was created. Once a completion operation was successfully
submitted, every outcome is delivered as exactly one terminal notice, including
cancellation and failure.

IOE-R1 names those source-private contracts directly:

- `IoEngineAdmissionResult` distinguishes Accepted, RejectedQuiescing, and
  RejectedShutdown for brand-new external work;
- `IoEngineOperationResult` distinguishes Accepted, invalid identity, missing
  registration, conflict, unsupported capability, and physical Shutdown;
- `registerOrUpdateReadiness` and `cancelReadiness` are owner-thread-only;
- `commitCompletionSubmission` is legal only after kernel acceptance and may
  acquire a storage lease;
- `commitCompletionCancellation` creates a drain obligation but never consumes
  the terminal packet;
- already-admitted owner work may clean up during Quiescing even though new
  external admission is sealed.

`IoEngineOptions::maxCompletionNoticesPerWait` is backend capacity. The IOE-R1
adapter compatibility-maps the stable `EventLoopOptions` IOCP field into it;
EventLoop callback/timer/control/functor budgets remain scheduler policy.

IOE-R2 gives Readiness its own source-private vocabulary and native owner:

- `ReadinessRegistrationIdentity` is the numeric source plus a nonzero,
  monotonically allocated generation; the generation is the value carried by
  each native epoll event instead of a raw Channel pointer;
- `ReadinessNotice` contains that identity, the borrowed Channel target, and a
  readiness mask; notices for one current generation may merge masks only
  within its current interests (error/close remain unconditional), while an
  unknown or replaced generation is counted stale and cannot escape the port;
- `EpollReadinessPort` owns epoll, its fixed native/notice batches, registration
  maps, and the internal eventfd wakeup. EventLoop owns dispatch and callbacks;
- register/update/cancel/wait remain owner-thread-only. `wakeup()` remains the
  sole cross-thread port method and carries no registration or business data;
- the first native implementation is bounded level-triggered epoll. Disabling
  and re-enabling interests retains one registration generation; remove and
  re-register always allocates a new generation;
- `IoEngineOptions::maxReadinessNoticesPerWait` is backend capacity, independent
  of EventLoop's `maxActiveChannelsPerIteration` dispatch budget.

IOE-C1's operation-model slice gives Completion its native result vocabulary:

- `CompletionOperationIdentity` is the operation address plus a nonzero
  submission generation. Preparation advances the generation, kernel
  acceptance commits it, synchronous non-pending failure rejects it, and only
  one terminal dequeue may retire it;
- `CompletionNotice` is a value result containing identity, operation kind,
  bytes, native error, terminal status, optional observer source/generation,
  and a source-private consumer. GQCSEx is decoded into this fixed typed batch
  before any scheduler dispatch;
- duplicate, rejected-generation, null, and already-terminal packets are
  invalid backend work. They produce no callback and cannot decrement a drain
  obligation;
- terminal dequeue retires kernel state and invokes only source-private
  terminal bookkeeping on the owner thread. Read/write driver, AcceptEx slot,
  and ConnectEx attempt state each own a separate consumer-terminal bit, so the
  same operation cannot be reused while an older typed notice from the current
  batch is still undispatched;
- every accepted observer-bound submission freezes its observer fd and a process-unique
  registration generation. EventLoop pulls notices under the same bounded I/O
  dispatch budget as readiness callbacks, revalidates fd, Channel identity,
  and the frozen generation, holds the Channel tie during a surviving callback,
  and invokes the source-private consumer even for a revoked or same-address
  replacement observer so driver retirement cannot be stranded;
- an operation lease retains only source-private shared transport, Accept-pool,
  or Connect-attempt state, not TcpConnection, Acceptor, or Connector application
  ownership. The entire typed-batch lease is released only after all direct
  notices and compatibility observers are consumed;
- the production Engine adapter publishes no Completion kind as fake Channel
  readiness. Accept/Connect/Read/Write all enter EventLoop through typed direct
  consumers. The legacy Poller shell may still translate notices only for its
  isolated compatibility contracts until that shell is retired.

## 7. Compatibility Sequence

1. IOE-R1 introduces a source-private Engine contract and an adapter around the
   existing Poller. No new installed header, public API, or observable behavior
   change is introduced.
2. IOE-R2 moves epoll/kqueue-style registration into a Readiness Engine while
   preserving Channel contracts and generation invalidation.
3. IOE-C1 moves IOCP delivery to typed Completion notices and removes fake
   Channel translation from the production Engine path before retiring the
   legacy Poller shell.
4. Only proven, cross-backend concepts may later graduate to a narrow public
   capability surface. Platform-specific controls remain source-private.

## 8. Test Contracts

- `tests/contract/io_engine/test_io_engine_poller_adapter.cpp` verifies that the
  source-private adapter reports native backend capabilities and capacity,
  rejects foreign-thread and invalid mutation, seals new admission, drains a
  committed completion cancellation, contains callback failure, invalidates a
  stale remove/re-register notice, continues a budgeted batch, follows the
  EventLoop lifecycle, and dispatches cross-thread wakeup on the owner loop.
- `tests/contract/io_engine/test_readiness_engine.cpp` verifies the concrete
  epoll port identity lifecycle, owner-only mutation, invalid request rejection,
  remove/re-register stale filtering, current-interest mask filtering,
  same-generation mask merge, bounded wait
  continuation, and internal cross-thread wakeup. On Windows it verifies that
  the same owner-loop Engine remains Completion-only.
- `tests/contract/io_engine/test_completion_engine.cpp` verifies operation
  identity and generation, distinct typed terminal notices, duplicate and
  rejected-generation filtering, source-private terminal bookkeeping,
  cancellation status, observer revocation, lease retirement, conflicting
  submission/cancellation rejection, direct owner dispatch, bounded
  continuation, generation revalidation after re-entry, and mandatory driver
  retirement for a revoked observer. On non-Windows it verifies that the
  completion vocabulary remains platform-neutral.
- `tests/contract/poller/test_poller_contract.cpp` verifies bounded backend
  waiting and backend-neutral Poller behavior during the adapter slice.
- `tests/contract/event_loop/test_event_loop.cpp` verifies owner-thread dispatch,
  wakeup, admission, and shutdown behavior.
- `tests/contract/event_loop/test_event_loop_fair_budget.cpp` verifies that I/O,
  timer, control, and pending-functor work remain bounded and make progress.
- `tests/contract/channel/test_channel_active_batch_lifetime.cpp` verifies
  generation-safe remove-before-destroy behavior during re-entrant dispatch.
- `tests/contract/tcp_connection/test_tcp_connection_completion_drain.cpp`
  verifies completion-obligation drain before loop exit.
- `tests/integration/tcp/test_iocp_accept_connect_quit_completion_drain.cpp`
  verifies real AcceptEx/ConnectEx completion retention on Windows.

Every Engine implementation slice adds a focused contract before replacing a
backend path. Cross-platform full CTest and the core benchmark are required for
integration; a local sample is directional evidence, not a release gate.

## 9. Non-Goals

- no HTTP, WebSocket, RPC, UDP, KCP, TLS, or coroutine expansion;
- no public backend selection or platform handle API in IOE-R1;
- no unbounded queue introduced to hide pressure;
- no normalization of completion into synthetic readiness after IOE-C1;
- no new business logic in EventLoop or the Engine.

## 10. Review Questions

- Is every mutation still owned by the EventLoop thread?
- Does the change preserve remove-before-destroy or terminal completion
  retirement, as appropriate?
- Can a callback re-enter and invalidate the next dispatch item safely?
- Is every cross-thread path marshaled through EventLoop admission?
- Which focused contract fails if this semantic boundary regresses?
