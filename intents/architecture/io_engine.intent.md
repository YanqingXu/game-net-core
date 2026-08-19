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
- Lifecycle phase publication is monotonic. Repeated `quit()` during
  Quiescing or FinalDraining is idempotent and cannot rewind the EventLoop or
  reopen any admission plane.

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
  notices are consumed;
- the production Engine adapter publishes no Completion kind as fake Channel
  readiness. Accept/Connect/Read/Write all enter EventLoop through typed direct
  consumers. The inherited Windows `Poller::poll()` ABI slot rejects use and
  cannot decode, publish, merge, or discard completion work;
- `IocpOperation` has no publication link, Acceptor has no Channel-queue
  fallback, and Channel implements no IOCP operation storage. The stable 0.3
  Channel header keeps three unreachable private pointer slots solely to avoid
  same-line fingerprint/layout churn; no production or test path initializes,
  reads, or writes them.

IOE-X1 authorizes one experimental Linux completion vertical slice:

- `GAMENET_ENABLE_EXPERIMENTAL=ON` on Linux builds a non-installed
  `GameNet::experimental` target containing a real raw-syscall io_uring
  Completion Engine. The default remains `OFF`; Windows rejects the option;
  the production Core continues to select epoll as its default and fallback;
- the first Engine is owner-thread-only and invokes no user callback. The owner
  explicitly enqueues, flushes, waits, pulls terminal notices, begins quiesce,
  and completes shutdown. A foreign thread receives an explicit owner error;
- only one-shot Accept, Recv, and Send are admitted. Every accepted operation
  receives a finite slot plus nonzero generation, owns its buffer/address
  storage and optional lease until exactly one terminal CQE, and becomes one
  typed success/failure/cancellation notice;
- the SQ, operation slots, completion batch, ready-notice count, per-operation
  payload, and total Engine-owned bytes are finite. A full native submission
  queue returns `SubmissionQueueFull`; it never blocks, allocates an unbounded
  overflow queue, or falls back to inline/readiness execution;
- `ASYNC_CANCEL` completion is only cancellation-request bookkeeping. The
  target operation's terminal CQE alone retires its slot and lease. Repeated
  cancellation is idempotent; stale slot generations cannot retire a reused
  operation;
- quiesce seals new one-shot work, flushes already-enqueued SQEs, submits
  bounded cancellation for every active operation, drains both target and
  internal cancel CQEs, and reaches physical Shutdown only when no staged SQE,
  active operation, or cancel completion remains;
- no multishot Accept/Recv, provided-buffer ring, buffer selection, registered
  or fixed file, zero-copy Send, SQPOLL, linked operation graph, public backend
  selector, or production TcpConnection integration is authorized in IOE-X1.

IOE-X2 authorizes one source-private EventLoop-driven completion pump:

- the pump remains inside the Linux-only, default-off, non-installed
  `GameNet::experimental` target. It does not enter the production Poller,
  TcpConnection, package export, or backend-selection surface;
- one EventLoop owner constructs, submits through, cancels through, drives, and
  destroys the pump. The pump owns one IOE-X1 Engine, one borrowed-fd Channel
  registration for the io_uring completion descriptor, and one source-private
  lifecycle participant; the caller owns and outlives both pump and EventLoop;
- normal progress is readiness-triggered by the ring descriptor, but each CQE
  remains a typed completion operation result. The Channel carries no
  operation identity, bytes, error, generation, lease, or terminal result;
- Engine wait is always nonblocking under EventLoop. One pump turn dispatches
  at most its validated `maxNoticesPerTurn`, independently from ring/CQ batch
  capacity, and commits a lifecycle continuation when decoded notices remain;
- `Accepted` submission retains its finite slot, generation, payload/address,
  and optional lease through exactly one terminal notice and source-private
  consumer return. Consumer code runs only on the owner and may re-enter
  owner-safe pump stop/cancel/submission APIs; admission is revalidated after
  every consumer return;
- a source-private quit participant is attached on the owner thread. The first
  EventLoop transition to Quiescing commits that participant callback without
  cross-thread allocation, even when `quit()` races or is called re-entrantly;
  this callback seals pump admission and begins cancellation before EventLoop
  may establish lifecycle silence;
- explicit quiesce and EventLoop quit share one idempotent path. SQ-full during
  final cancellation is flushed and retried only within the existing finite
  ring; ASYNC_CANCEL remains bookkeeping and the target CQE alone retires the
  lease. Every cancellation terminal notice is still delivered to the
  source-private consumer;
- physical pump stop, Channel removal, lifecycle detach, and stop-future
  publication occur only after active operations, staged submissions, cancel
  CQEs, decoded notices, and callback frames are all silent. A wait/flush or
  consumer failure is counted and fails closed: the pump continues bounded
  nonblocking final-drain turns and cannot publish a false drained result or
  permit EventLoop Shutdown while a kernel obligation remains;
- IOE-X2 does not authorize blocking owner waits, a second worker thread,
  per-operation functor posts, fake readiness results, multishot, provided
  buffers, fixed files, zero-copy, SQPOLL, or production TCP integration.

IOE-X3 authorizes one experimental single-connection Completion TCP driver:

- `IoUringTcpConnectionDriver` remains Linux-only, default-off, non-installed,
  and source-private. It composes one IOE-X2 Pump for one already-established
  stream socket; it does not modify or substitute production `TcpConnection`;
- the constructing EventLoop thread is the only mutation and callback owner.
  Construction transfers unique socket ownership to the driver, the caller
  owns and outlives the driver and EventLoop, retains the driver through every
  message/close consumer return, and uses no direct cross-thread driver method;
- `start()` posts exactly one one-shot Recv. At most one Recv identity may be
  active. A successful terminal Recv clears that identity before invoking the
  message consumer, and a replacement is posted only after re-entry is
  revalidated;
- read pause means `readDesired == false`: it requests cancellation of an
  already-submitted Recv when possible and never posts a replacement. Resume
  during cancellation records desire only; the target terminal CQE must retire
  before exactly one replacement Recv may be submitted;
- send admission owns a copied segment only within explicit byte and segment
  limits. At most one Send identity is active; queued segments preserve FIFO,
  partial terminal results advance the exact front offset, and `Accepted`
  bytes are either sent or terminally discarded by an observable close;
- peer EOF, terminal Recv/Send failure, user callback failure, EventLoop
  quiesce, or explicit close chooses one first close reason. Closing revokes
  read repost and new send admission, asks the Pump to seal/cancel, and keeps
  the socket plus all operation state alive through every target terminal CQE;
- the Pump's physical stopped hook runs only after Engine shutdown, Channel
  removal, lifecycle detach, decoded-notice retirement, and consumer return.
  Only that hook closes the socket, publishes the driver stop future, and
  invokes the single close consumer. Close-consumer re-entry observes Stopped;
- IOE-X3 does not authorize Accept/listen integration, production
  `TcpConnection` changes, a public connection/backend selector, worker-thread
  fallback, unbounded output, multishot, provided buffers, fixed files,
  zero-copy, SQPOLL, TLS, framing, or game/business callbacks in Core.

## 7. Compatibility Sequence

1. IOE-R1 introduces a source-private Engine contract and an adapter around the
   existing Poller. No new installed header, public API, or observable behavior
   change is introduced.
2. IOE-R2 moves epoll/kqueue-style registration into a Readiness Engine while
   preserving Channel contracts and generation invalidation.
3. IOE-C1 moves IOCP delivery to typed Completion notices, removes fake
   Channel translation from both production and compatibility paths, and makes
   repeated shutdown requests phase-monotonic.
4. IOE-X1 proves the same terminal-operation and final-drain invariants through
   a default-off, non-installed real io_uring path while epoll stays production
   default/fallback.
5. IOE-X2 proves that the isolated one-shot Engine can participate in the
   existing EventLoop owner/fairness/final-drain lifecycle without becoming a
   Poller backend or changing production TCP.
6. IOE-X3 proves connection-level one-Recv, bounded FIFO Send, pause/resume,
   re-entry, and close retirement over that Pump for one established socket,
   still without changing production TCP or exposing backend selection.
7. Only proven, cross-backend concepts may later graduate to a narrow public
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
  waiting, distinct direct consumer dispatch, no fake Channel callbacks, and
  backend-neutral Poller behavior.
- `tests/contract/event_loop/test_event_loop.cpp` verifies owner-thread dispatch,
  wakeup, admission, and shutdown behavior.
- `tests/contract/event_loop/test_event_loop_lifecycle_hub.cpp` verifies the
  observable Running/Quiescing/FinalDraining/Shutdown sequence and re-entrant
  `quit()` monotonicity in FinalDraining.
- `tests/contract/event_loop/test_event_loop_fair_budget.cpp` verifies that I/O,
  timer, control, and pending-functor work remain bounded and make progress.
- `tests/contract/channel/test_channel_active_batch_lifetime.cpp` verifies
  generation-safe remove-before-destroy behavior during re-entrant dispatch.
- `tests/contract/tcp_connection/test_tcp_connection_completion_drain.cpp`
  verifies completion-obligation drain before loop exit.
- `tests/integration/tcp/test_iocp_accept_connect_quit_completion_drain.cpp`
  verifies real AcceptEx/ConnectEx completion retention on Windows.
- `tests/integration/tcp/test_iocp_quit_completion_drain.cpp` verifies a real
  cancellation terminal packet is consumed while the loop is Quiescing and
  before Shutdown publication.
- `tests/contract/io_engine/test_io_uring_completion_engine.cpp` verifies a
  real Linux one-shot Accept/Recv/Send round trip, finite-SQ rejection,
  owner-thread enforcement, operation generation, cancellation CQE separation,
  lease retention, and bounded final-drain convergence.
- `tests/contract/io_engine/test_io_uring_event_loop_pump.cpp` verifies that a
  real ring descriptor drives typed Recv/Send completions without manual wait,
  that EventLoop quit automatically seals and cancels a pending operation,
  that its lease survives through the cancellation consumer, and that pump
  retirement precedes EventLoop Shutdown with bounded dispatch and zero
  residual operation/notice state.
- `tests/contract/io_engine/test_io_uring_tcp_connection_driver.cpp` verifies
  one real established stream socket, one-Recv-in-flight, pause/no-repost and
  resume-after-terminal behavior, finite FIFO Send admission, callback
  re-entry, pending-Recv cancellation, first-close-reason, socket release only
  after physical Pump stop, foreign-thread rejection, and zero residual state.
- `tests/contract/event_loop/test_event_loop_lifecycle_hub.cpp` verifies the
  source-private quit participant is committed exactly once on the first
  Running-to-Quiescing transition, can self-signal during final drain, and is
  not activated by repeated quit after Shutdown.
- `tests/cmake/test_io_uring_completion_engine_contract.py` guards the
  default-off/non-installed Linux target, epoll fallback, raw-kernel operation
  allowlist, explicit finite budgets, CI execution, and rejection of multishot,
  provided buffers, fixed files, zero-copy, and SQPOLL.
- `benchmarks/io_uring/one_shot.cpp` is an opt-in, non-CTest directional
  benchmark. It keeps a finite number of one-shot Send/Recv operations in
  flight over a real socket, validates every terminal notice, and reports
  message/operation throughput, P50/P99/P999 latency, owned-byte convergence,
  shutdown latency, SQ-full rejection, and cross-domain fallback counts. It is
  evidence for the isolated experimental Engine, not permission to replace the
  production epoll path.

Every Engine implementation slice adds a focused contract before replacing a
backend path. Cross-platform full CTest and the core benchmark are required for
integration; a local sample is directional evidence, not a release gate.

## 9. Non-Goals

- no HTTP, WebSocket, RPC, UDP, KCP, TLS, or coroutine expansion;
- no public backend selection or platform handle API in IOE-R1;
- no unbounded queue introduced to hide pressure;
- no normalization of completion into synthetic readiness after IOE-C1;
- no production-backend replacement or advanced io_uring feature in IOE-X1;
- no new business logic in EventLoop or the Engine.

## 10. Review Questions

- Is every mutation still owned by the EventLoop thread?
- Does the change preserve remove-before-destroy or terminal completion
  retirement, as appropriate?
- Can a callback re-enter and invalidate the next dispatch item safely?
- Is every cross-thread path marshaled through EventLoop admission?
- Which focused contract fails if this semantic boundary regresses?
