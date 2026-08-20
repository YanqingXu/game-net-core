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

IOE-X4 authorizes one experimental shared-Pump connection hub:

- `IoUringTcpConnectionHub` remains Linux-only, default-off, non-installed,
  and source-private. One EventLoop owns exactly one IOE-X2 Pump/Engine and a
  fixed-capacity set of at least two already-established TCP connections; no
  connection owns a separate ring, Channel, wait loop, or worker thread;
- every admitted connection receives a slot plus nonzero generation. Slot
  reuse advances generation. A fixed operation-route table indexed by Engine
  operation slot stores the exact operation generation, connection identity,
  and kind; a stale connection or operation generation invokes no callback and
  can retire no current route;
- an Engine operation slot remains reserved while its terminal notice is
  queued and becomes reusable only when that exact generation is taken for
  dispatch. This keeps the fixed Hub route entry unambiguous even when one wait
  decodes more notices than the Pump dispatches in its current turn;
- each route uniquely owns its transferred socket, callbacks, one Recv and one
  Send identity, finite FIFO segments/bytes, first close reason, metrics, and
  stop promise. Per-route byte/segment admission and a separate finite Hub byte
  budget are checked without overflow; one saturated route cannot consume or
  cancel another route's reservations;
- connection-local close revokes that route's admission, discards only its
  queued suffix, and cancels only its active identities. It never quiesces the
  shared Pump. The socket/slot generation are released only after both target
  terminals retire; another route continues to receive, send, and callback;
- pause/resume and callback re-entry preserve the IOE-X3 one-Recv rule per
  route. Terminal routing clears operation and route identities before user
  code. Close callbacks are deferred until the outer completion consumer has
  returned and may re-enter owner-safe Hub methods without recursive callback;
- the Hub owns one normal lifecycle maintenance source solely to retry a
  bounded cancellation that temporarily found the SQ full. Maintenance never
  carries completion data or invokes user code. It is detached before Hub stop
  publication;
- explicit Hub stop or EventLoop Quiescing closes admission, chooses one close
  reason for every live route, and quiesces the shared Pump exactly once. Hub
  stop is published only after every connection future, socket, operation
  route, queued byte, Engine CQE/cancel obligation, Channel, and lifecycle
  source has converged;
- IOE-X4 does not authorize Accept/listen ownership, production
  `TcpConnection` integration, a public backend selector, dynamic/unbounded
  routing, cross-owner connection migration, multishot, provided buffers,
  fixed files, zero-copy, SQPOLL, TLS, framing, or game/business state in Core.

IOE-X5 authorizes capacity, churn, and directional measurement of that same
experimental shared-Pump Hub without expanding its production authority:

- one direct contract fixes 256 simultaneously admitted real loopback TCP
  routes on one EventLoop/Pump/Engine. It closes and generation-replaces 64
  routes while the other 192 retain their identities and must make receive and
  send progress after replacement begins;
- the contract keeps the connection table, operation table, receive bytes,
  per-route send bytes/segments, aggregate send bytes, Pump dispatch, and
  EventLoop lifecycle budgets explicit and finite. Every old identity is stale,
  every accepted byte is sent or discarded, and all 320 old/replacement route
  futures, sockets, operation entries, and byte reservations converge before
  Hub stop;
- the capacity contract remains one bounded test, not an unbounded connection
  factory or a production listener. Peers own their non-Hub socket ends and
  transfer only the established server ends to the Hub;
- a separate opt-in, non-CTest, Release-oriented shared-Hub benchmark may keep
  one fixed-size framed echo in flight per route and report connection count,
  completed round trips, throughput, P50/P99/P999 latency, working-set delta,
  bytes per connection, shutdown latency, operation/connection high-water
  marks, rejection counts, and zero-residue metrics in validated JSON;
- benchmark and production epoll samples are directional because their
  transport and dispatch topology differ. IOE-X5 must publish an explicit
  `PROMOTE` or `DEFER` decision; a local numeric advantage alone cannot authorize
  a public selector or production `TcpConnection` integration;
- IOE-X5 still forbids Accept/listen ownership inside the Hub, dynamic route
  growth, cross-owner migration, multishot, provided buffers, fixed files,
  zero-copy, SQPOLL, TLS, framing in Core, or game/business callbacks.

IOE-X6 authorizes one non-installed TCP semantic adapter over a Hub route. It
is contract shaping for a future production adapter, not backend selection:

- the adapter and Hub remain owned by one EventLoop. An established socket is
  transferred exactly once to the Hub; the adapter may be destroyed while a
  route is closing because observer lifetime is separated from the Hub's
  socket, operation, byte, and terminal-future obligations;
- send uses the existing `TcpSendResult` vocabulary. Empty send is accepted
  only while open, a connection-local hard limit returns `Overloaded`, Hub
  aggregate pressure returns `LoopOverloaded`, and a closing/stale route
  returns `Closed`; no rejection creates deferred work or a fallback queue;
- adapter low/high/hard thresholds are finite. Crossing high pauses the Hub
  route's Recv and schedules one owner-loop high-water notification; falling
  to low resumes exactly one Recv. Reaching zero schedules one write-complete
  notification. Notification admission is bounded and drops are observable;
- message, high-water, write-complete, close-info, and close callbacks execute
  only on the owner loop, may re-enter adapter methods, and are exception
  contained. Callback failure closes that route without escaping Pump dispatch;
- the first semantic `TcpConnectionCloseInfo` wins. Peer EOF, forced close,
  callback failure, reset-class native errors, owner quiescence, and internal
  failure are mapped without erasing the native error. Adapter terminal future
  publication follows physical Hub socket/operation retirement and precedes
  close callbacks;
- one Linux contract drives production epoll `TcpConnection` and this adapter
  through the same real-TCP saturation, read-pause/resume, callback-reentrant
  send/force-close, and terminal-observation scenario. It compares the common
  semantic boundary, not backend-specific scheduling or throughput;
- IOE-X6 does not add Accept/listen ownership, cross-thread adapter mutation,
  graceful half-close, public backend selection, installed headers, or change
  production `TcpConnection`. Missing graceful-drain equivalence remains an
  explicit later contract rather than being faked as forced close.

IOE-X7 authorizes that missing graceful-drain and half-close contract inside
the same non-installed Hub/Adapter boundary:

- `tryShutdown` seals new route send admission and publishes
  `GracefulShutdown` as the first semantic reason. Every send accepted before
  that call remains owned and must be fully sent; it cannot be reclassified as
  discarded merely because graceful shutdown began;
- once the route's accepted output reaches zero, the owner executes exactly one
  native `shutdownWrite`. The route keeps its receive desire and at most one
  Recv active, may deliver peer data after local half-close, and reaches its
  terminal close only on peer EOF, reset/failure, explicit force escalation, or
  owner/Hub quiescence;
- repeated graceful requests are idempotent. Force escalation may cancel the
  still-pending Recv and accelerate physical close, but first-close-reason wins:
  an earlier `GracefulShutdown` remains the Adapter close info;
- Hub metrics and route summaries expose graceful request and write-half-close
  counts. Terminal publication still follows target CQE retirement, socket
  close, accepted-byte reconciliation, and precedes Adapter close callbacks;
- a real-TCP contract drives production epoll `TcpConnection` and the Adapter
  through accepted pending output, graceful request, peer-observed full payload
  then EOF, peer reply after local EOF, peer half-close, and identical graceful
  terminal observation. A second route proves forced escalation preserves the
  first graceful reason without stranding Recv/Send work;
- IOE-X7 remains owner-thread-only, default-off, and non-installed. It does not
  authorize Accept/listen ownership, cross-thread Adapter calls, public backend
  selection, production `TcpConnection` changes, or advanced io_uring features.

IOE-X8 authorizes bounded cross-thread command admission at that Adapter
boundary without moving Hub or connection ownership:

- callback configuration, socket establishment, state/metric queries, and
  Adapter destruction remain owner-loop-only. Only `trySend`, `tryShutdown`,
  and `tryForceClose` may be called from a foreign thread, and the caller must
  keep the Adapter facade alive until each call returns;
- one fixed-capacity Adapter command mailbox serializes owner and foreign
  Send/Shutdown/Force commands. One source-private EventLoop lifecycle signal
  coalesces wakeups and drains at most a configured command budget per turn;
  Hub mutation, socket syscalls, callbacks, and terminal publication remain on
  the original EventLoop owner;
- a non-empty foreign send copies its payload before returning `Accepted` and
  atomically reserves it against the Adapter hard byte limit and command-slot
  limit. Hard pressure returns `Overloaded`; mailbox pressure returns
  `SchedulingQueueFull`; a terminal-sealed connection returns `Closed`, while
  EventLoop admission closure maps to `OwnerShutdown` or `OwnerUnavailable`.
  Rejection owns no payload and creates no deferred work;
- command admission order is the lifecycle order. Every send accepted before
  the first accepted Shutdown/Force command is delivered to the owner before
  that terminal command. The first terminal admission seals later sends;
  repeated terminal requests are idempotent and first-close-reason still wins;
- `Accepted` transfers the command and copied payload obligation to shared
  Adapter state, not to the observer facade. The command must reach Hub send or
  be reconciled as cancelled bytes by one observable connection terminal.
  Owner quit drains the signalled mailbox before physical Hub retirement;
- callback re-entry uses the same mailbox ordering. Owner-side calls may drain
  already accepted foreign commands first, but no callback executes off-owner
  and no caller blocks waiting for the EventLoop;
- owner-side Adapter destruction requires all active facade calls to have
  returned, revokes the observer, seals the mailbox, reconciles queued sends,
  and requests physical route close. Already accepted commands retain no raw
  facade pointer and cannot callback after observer destruction;
- a real-TCP contract drives production epoll `TcpConnection` and the Adapter
  from foreign callers through ordered send then graceful shutdown, mailbox
  saturation/recovery, concurrent shutdown/force races, observer revocation,
  and owner quit. It asserts typed rejection, one terminal, accepted-byte
  reconciliation, callback owner affinity, and zero queue/Hub/Engine residue;
- IOE-X8 remains Linux-only, default-off, source-private, and non-installed. It
  does not authorize listener/Accept ownership, public backend selection,
  production `TcpConnection` changes, or advanced io_uring operations.

IOE-X9 authorizes one source-private listener inside the existing shared Hub;
it does not create a second Pump or move connection ownership:

- a valid, already-bound/listening stream socket transfers to the Hub on every
  `listen` call, including synchronous rejection. One Hub owns at most one
  listener lifetime, its socket, callback factory, Accept identities, metrics,
  close reason, native error, and stop promise. Listener configuration and all
  lifecycle calls remain owner-loop-only;
- `maxPendingAccepts` is a positive finite one-shot Accept window. Every
  accepted submission is generation-bound in the Hub's existing fixed
  operation-route table as `Accept`; Pump/Engine budgets bound CQ decoding and
  per-turn dispatch, so listener progress adds no unbounded queue, worker,
  Channel, ring, or recursive drain;
- a successful Accept notice owns its nonblocking/CLOEXEC fd until the owner
  Hub consumes it. The connection factory returns callbacks only and never
  sees or owns the fd. The Hub establishes RAII before fallible work, clears
  the exact Accept identity before factory re-entry, revalidates listener/Hub
  phase afterward, and transfers the fd directly into `addConnection` only
  when admission remains open. Capacity, invalid-factory, callback, quiesce,
  and Engine rejection paths close the accepted fd exactly once;
- connection capacity rejection is local and observable, then the listener
  rearms. `EINTR`, `ECONNABORTED`, and `EPROTO` are bounded transient Accept
  terminals and rearm; other Accept failure, factory exception/invalid return,
  or inability to arm the first one-shot closes the listener fail-closed with
  typed result/reason/native error while already-established routes remain
  owned by the Hub;
- `stopListening` first seals admission and closes the listening socket, then
  cancels every exact pending Accept. Its future is published only after all
  Accept identities retire and proves listener socket close plus zero active
  Accepts. Hub stop performs that listener-first transition before closing
  connection routes and quiescing the shared Pump; the listener future is
  ready no later than the Hub stop future;
- a real-TCP contract drives production epoll `TcpServer` and the Hub listener
  through bind/listen, connect, echo, peer close, and graceful server stop. Hub
  cases additionally cover a finite concurrent Accept window, connection-limit
  rejection/recovery, generation reuse across burst/churn, callback re-entry,
  deterministic Engine-operation pressure, explicit listener stop, and owner
  quit with zero listener/route/operation/notice/fd/byte residue;
- IOE-X9 remains Linux-only, default-off, source-private, and non-installed.
  It does not authorize public listener APIs, backend selection, production
  `Acceptor`/`TcpServer` changes, multishot Accept, provided buffers, or other
  advanced io_uring features.

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
7. IOE-X4 replaces the proof-only per-connection Pump topology with one finite
   owner Hub that generation-routes multiple connection operations and proves
   isolated close plus aggregate owner shutdown.
8. IOE-X5 subjects that Hub to fixed 256-route churn, soak, and directional
   measurement before recording a production-adapter promotion decision.
9. IOE-X6 shapes a non-installed per-route semantic adapter and proves its
   common forced-close/backpressure boundary beside production epoll without
   selecting or replacing a backend.
10. IOE-X7 adds pending-output graceful drain, one native write half-close,
   continued read, peer-EOF terminal, and first-reason-preserving escalation.
11. IOE-X8 adds bounded cross-thread Send/Shutdown/Force admission while the
    Adapter, Hub, socket, callbacks, and retirement remain on one owner loop.
12. IOE-X9 moves one finite one-shot listener window into the existing shared
    Hub/Pump and proves accepted-fd transfer plus listener-first stop.
13. Only proven, cross-backend concepts may later graduate to a narrow public
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
- `tests/contract/io_engine/test_io_uring_tcp_connection_hub.cpp` verifies one
  Pump routes two concurrent real TCP connections, per-route and Hub budgets,
  isolated connection close, slot-generation reuse with stale-handle rejection,
  continued neighbor progress, callback-safe replacement, explicit Hub stop,
  EventLoop-quit aggregate cancellation, and zero connection/operation/byte
  residue before Hub stop publication.
- `tests/contract/io_engine/test_io_uring_tcp_connection_hub_capacity.cpp`
  verifies one shared Pump holds 256 real TCP routes, generation-replaces 64
  from close callbacks, preserves post-churn progress for the other 192, and
  drains all 320 route lifetimes plus fixed operation/byte state to zero.
- `tests/contract/io_engine/test_io_uring_tcp_connection_adapter.cpp` drives
  production epoll TcpConnection and the IOE-X6 adapter through matching real
  TCP output saturation, read pause/resume, callback re-entry, forced close,
  first close info, socket-before-future retirement, and post-observer drain;
  IOE-X7 extends it with pending-output graceful drain, peer-observed local EOF,
  continued inbound delivery, peer EOF terminal, and force escalation that
  cannot replace the first graceful close info; IOE-X8 extends the same real
  TCP boundary with foreign send/shutdown/force order, bounded mailbox
  saturation, observer revocation, owner quit, and zero command residue.
- `tests/contract/io_engine/test_io_uring_tcp_listener.cpp` drives production
  epoll and the shared-Pump listener through real bind/connect/echo/stop, then
  verifies finite Accept depth, capacity rejection/recovery, generation churn,
  first-arm Engine pressure, callback re-entry, explicit stop, owner quit,
  accepted-fd reconciliation, and zero listener/Hub/Engine residue.
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
