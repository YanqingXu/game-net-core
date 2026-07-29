---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: EventLoop

## 1. Intent
EventLoop is the core event-dispatch engine bound to one thread.
It waits for active I/O, dispatches Channel callbacks, executes queued tasks,
and serves as the central scheduling point for timer and coroutine extensions.

EventLoop is the heart of reactor execution in game-net-core.

---

## 2. Responsibilities
- own and drive Poller
- perform poll wait loop
- dispatch active Channel events
- execute pending functors submitted from same thread or other threads
- bound pending-functor admission and execute at most a configured batch per
  normal loop iteration so task floods cannot grow memory without limit or
  monopolize the owner thread
- host a separately bounded set of pre-registered internal control sources;
  each source owns one coalescing mailbox bit that cannot be consumed by
  normal or reserved pending functors
- own one dynamic lifecycle hub for runtime-created reactor participants;
  lifecycle nodes attach/detach on the owner thread while cross-thread signals
  use an intrusive dirty set and allocate no queue node
- drive the explicit loop shutdown state machine
  `Running -> Quiescing -> FinalDraining -> Shutdown`
- during Windows quiescing, continue zero-timeout IOCP polling until both
  backend completion obligations and lifecycle-hub work are silent
- maintain thread-affinity discipline
- provide runInLoop / queueInLoop API
- provide a copyable, non-owning `EventLoopExecutor` that can reject queued work
  after loop admission closes without exposing a dereferenceable raw loop
- support wakeup mechanism for cross-thread responsiveness
- provide safe extension point for timers and coroutine resume
- contain exceptions from asynchronously dispatched Channel, Timer, pending-
  functor, and metric callbacks behind an observable owner-loop policy

---

## 3. Non-Responsibilities
- does not parse business protocols
- does not own all connection objects by itself
- does not define application-level routing logic
- does not replace a thread pool abstraction
- does not directly implement high-level coroutine task graph in v1

---

## 4. Core Invariants
- one EventLoop binds to exactly one thread
- loop() runs only on owner thread
- Poller is used only by owner thread
- pending functors execute on owner thread
- quit must not abandon already-queued pending functors
- wakeup is used to interrupt blocking poll when needed
- channel update/remove must respect EventLoop ownership
- every active Channel batch has one owner-thread epoch; each unique Channel is
  assigned one O(1)-addressable slot before callbacks begin
- successful Channel removal invalidates that slot before ownership may be
  released, and dispatch skips invalidated slots without dereferencing them
- the loop may temporarily own one already-removed current Channel solely
  until its active callback frame returns; this source-private retirement path
  performs no queue admission or allocation
- Poller lifetime does not exceed EventLoop lifetime
- EventLoop exclusively owns executor admission state; executor handles neither
  own nor extend EventLoop lifetime
- `quit()` closes executor admission before the final pending-functor drain, so
  a successful executor post is either in that drain set or a normal loop
  iteration and a later post returns `PostResult::Shutdown`
- owner-thread identity remains usable only while accepting work or executing
  that final accepted-work drain; it becomes unavailable after the drain ends
- once the loop has published control-plane Shutdown, repeated or concurrent
  `quit()` calls are idempotent and cannot re-enable the executor's
  final-drain owner identity
- capacity-aware `tryQueueInLoop` and executor submissions never exceed the
  normal pending-functor limit; legacy `queueInLoop` may use only the
  separately bounded reserve and throws explicitly when total capacity is full
- `queueInLoop` and its reserve remain legacy/data-plane facilities and do not
  provide lifecycle progress guarantees; internal lifecycle work uses a
  pre-registered control source or a dynamic lifecycle node instead
- control-source registration is owner-thread-only, happens before `loop()`,
  cannot exceed the configured `maxControlSources`, and rejects configurations
  above the supported 65,536-source storage bound before allocation
- registration and unregistration are reachable only through the source-private
  `detail::EventLoopControlRegistry`; installed public callers cannot reserve
  slots or register arbitrary work in the internal control lane
- a registered source has at most one pending mailbox bit; repeated `notify()`
  calls coalesce without allocating or growing a queue
- normal-plus-reserved saturation cannot reject a registered control-source
  notification
- control callbacks execute on the owner thread, after the active Channel and
  expired-timer phases, and before normal pending functors
- one control source executes at most once per control round; a callback that
  notifies itself is deferred to a later round and never recursively invoked
- quit linearizes the control plane from Accepting to Draining: notifications
  accepted before that transition are included in the final drain, external
  notifications after it return `PostResult::Shutdown`, and only the currently
  executing control callback may re-arm its own source while the accepted-work
  drain is running
- the control plane becomes Shutdown only after its pending bitset is empty;
  handles whose EventLoop no longer exists return
  `PostResult::OwnerUnavailable`
- runtime-created lifecycle participants attach through the source-private
  lifecycle registry on the owner thread; attach is fallible and bounded by
  `maxLifecycleNodes`, and allocation happens only during attach
- a lifecycle source contains a node generation. `signal()` takes no
  allocation path, and under the hub lock either commits the matching
  generation to the intrusive dirty set or returns Shutdown/OwnerUnavailable
- `PostResult::Accepted` from lifecycle `signal()` is a committed-notify
  guarantee: the matching callback executes at least once before that
  generation can be reclaimed, even when detach or quit races the signal
- repeated signals for one dirty generation coalesce; a callback that signals
  itself is deferred to a later lifecycle round and is never invoked
  recursively
- detach is owner-thread-only, invalidates the generation before returning,
  and stale copied sources cannot dirty a replacement node (ABA protection)
- detach never releases callback/node storage while a committed notification
  or callback frame for that generation remains outstanding; reclamation
  occurs after the owner-thread drain observes both detached and clean
- lifecycle draining is budgeted by
  `maxLifecycleCallbacksPerIteration`; remaining dirty nodes request another
  loop turn through wakeup/self-reschedule without using the pending-functor
  queue
- lifecycle callbacks are internal control work, not user callbacks, and must
  be idempotent because multiple external events may coalesce into one visit
- quit first publishes Quiescing and seals new external executor/control/
  lifecycle admission while preserving signals committed before the
  transition
- Quiescing continues I/O completion consumption and lifecycle draining;
  FinalDraining begins only after no backend completion obligation and no
  lifecycle dirty/detaching node remains
- retained IOCP operation storage alone does not prolong shutdown. An adapter
  canceling a successfully submitted operation must separately mark its
  shutdown completion obligation; dequeue releases that obligation exactly
  once even after the Channel observer has been revoked
- FinalDraining executes already-accepted normal functors and permitted
  self-rearmed control/lifecycle work to a fixed point; Shutdown is published
  only after all three admission planes are closed and silent
- normal iterations execute at most `maxFunctorsPerIteration`; accepted
  remainder is preserved and the loop is woken for another I/O/timer-aware
  iteration
- active Channel, expired-timer, registered-control, lifecycle, and pending-
  functor phases each execute at most their validated per-iteration count
  budget. A partially dispatched active batch remains owner-loop storage, and
  removal from any intervening phase invalidates its O(1) slot before release
- expired timers beyond the current budget remain in TimerQueue's ordered
  owner-loop container, forcing a zero poll timeout; control bits beyond the
  current budget remain in the fixed mailbox, and both paths continue without
  pending-functor admission
- append-only scheduler metrics report the phase's drained count, exact ready
  remainder, oldest-ready lag, and whether the count budget was exhausted
- Windows wakeup producers share one Poller-owned atomic pending bit. Exactly
  one producer that changes it from clear to pending posts an IOCP wakeup
  packet; later producers merge into that pending turn without allocation
- the owner clears the Windows wakeup pending bit immediately after dequeuing
  its packet and continues translating the rest of the same completion batch.
  Work published by a producer before that clear is visible to the already
  awake iteration; a producer after the clear posts the next packet, so the
  clear/producer race cannot strand accepted work
- logical `wakeup()` observations remain distinct from physical IOCP packets:
  metrics count every scheduling request even when the backend coalesces its
  packet
- executor/control/lifecycle admission synchronization keeps every permitted
  cross-thread wakeup call inside EventLoop/Poller lifetime; after admission
  publishes Shutdown no scheduling handle may access a destroyed completion
  port
- Windows IOCP polling returns at most one fixed 64-packet completion batch per
  normal loop iteration; an I/O backlog therefore yields to expired timers,
  control sources, lifecycle nodes, and pending functors before the next batch
- one asynchronous callback exception never skips later ready timers or
  already-accepted pending functors, and Channel dispatch always clears its
  in-callback lifetime guard while unwinding

---

## 5. Collaboration
- owns one Poller
- owns one TimerQueue
- coordinates with Channel objects for I/O event dispatch
- provides scheduling point for timer callbacks and coroutine awaiter resume
- interacts with EventLoopThread / thread-pool wrappers in scaled mode

---

## 6. Main Execution Model
Default v1 loop direction:
1. wait for active I/O via Poller
2. stamp and dispatch one budgeted portion of the unique active-Channel batch,
   skipping registrations invalidated by earlier or intervening callbacks
3. dispatch one budgeted portion of expired timers
4. execute one budgeted round of pending control sources
5. execute one budgeted round of dirty lifecycle nodes
6. execute pending functors
7. repeat until quit requested

If quit is observed, EventLoop enters Quiescing. Linux may use a zero-timeout
poll to establish backend silence; Windows must continue zero-timeout IOCP
polling while retained or participant-owned completion obligations exist.
Only after backend and lifecycle silence may the loop enter FinalDraining and
drain already-accepted pending functors plus committed control/lifecycle work
before publishing Shutdown.

If this order changes, docs/tests/contracts must be updated.

---

## 7. Public API Direction
Typical API direction:
- loop()
- quit()
- runInLoop(Functor)
- queueInLoop(Functor)
- tryQueueInLoop(Functor) -> bool
- source-private detail::EventLoopControlRegistry::registerSource(Functor)
  -> EventLoopControlSource
- source-private detail::EventLoopControlRegistry::unregisterSource(
  EventLoopControlSource)
- EventLoopControlSource::notify() -> PostResult
- source-private detail::EventLoopLifecycleRegistry::attach(Functor)
  -> EventLoopLifecycleSource
- source-private detail::EventLoopLifecycleRegistry::detach(
  EventLoopLifecycleSource)
- EventLoopLifecycleSource::signal() -> PostResult
- pendingFunctorCount() / rejectedFunctorCount() snapshots
- pendingControlSourceCount() / mergedControlNotificationCount() snapshots
- attachedLifecycleNodeCount() / pendingLifecycleNodeCount() /
  mergedLifecycleSignalCount() snapshots
- phase() -> EventLoopPhase
- executor() -> EventLoopExecutor
- setCallbackExceptionHandler(EventLoopCallbackExceptionHandler)
- EventLoopOptions fairness budgets:
  `maxActiveChannelsPerIteration`, `maxTimersPerIteration`,
  `maxControlCallbacksPerIteration`, `maxLifecycleCallbacksPerIteration`, and
  `maxFunctorsPerIteration`
- callbackExceptionCount()
- runAt(Timestamp, Functor)
- runAfter(Duration, Functor)
- runEvery(Duration, Functor)
- cancel(TimerId)
- updateChannel(Channel*)
- removeChannel(Channel*)
- assertInLoopThread()
- isInLoopThread()
- wakeup()

Additional APIs can be added later for richer timer and coroutine features.

---

## 8. Threading Rules
- EventLoop has owner thread identity
- same-thread runInLoop executes immediately
- cross-thread runInLoop behaves like queued scheduling
- queueInLoop enqueues within the normal-plus-reserved hard capacity and throws
  explicitly at saturation
- tryQueueInLoop and EventLoopExecutor return false at normal-capacity saturation
- control-source registration and unregistration are owner-thread-only;
  `notify()` is thread-safe and non-throwing
- lifecycle attach/detach are owner-thread-only; `signal()` is thread-safe,
  non-throwing, allocation-free, and generation checked
- control-source callbacks are owner-thread-only and user/data work must not be
  routed through this internal lifecycle facility
- lifecycle callbacks execute only on the owner thread; callback targets are
  retained by hub-owned node storage until detach reclamation
- cross-thread enqueue must ensure wakeup when loop may be blocked
- pending functor queue flush occurs on owner thread only
- cross-thread-observed pending functor execution state is atomic or synchronized

---

## 9. Wakeup Intent
Wakeup exists to solve a specific problem:
the loop may be blocked in poll while another thread submits new work.

Wakeup should:
- be explicit
- be lightweight
- not become a hidden data channel
- not replace normal callback/business delivery semantics

---

## 10. Failure Semantics
EventLoop should explicitly handle:
- backend poll errors/interruption
- wakeup read/write issues
- invalid non-owner-thread mutation attempts
- asynchronous callback exceptions are counted and reported with their source;
  the default action logs and continues, while an installed policy may request
  loop quit without abandoning already-accepted pending functors
- an exception thrown by the exception handler itself is logged and requests
  loop quit rather than recursing through the same handler
- quit request while processing current iteration
- queued functors that were already accepted before loop exit
- executor `tryQueue` after admission close or destruction returns false and
  never dereferences the expired EventLoop
- typed executor `post` after quit returns `PostResult::Shutdown`; a post that
  linearized before quit returns Accepted and remains in the final drain
- a `quit()` arriving after final drain observes Shutdown, leaves
  `drainingAccepted` false, and cannot make `isInOwnerThread()` true again
- pending-functor saturation is explicit: capacity-aware APIs return false,
  while the legacy API throws `std::overflow_error`
- control-source registration capacity exhaustion fails before the loop starts;
  an already registered source never reports QueueFull at runtime
- `PostResult::Accepted` means the source bit is either newly pending or safely
  coalesced into an existing pending request
- `PostResult::Shutdown` means quit has sealed external control admission;
  `PostResult::OwnerUnavailable` means the source registration or EventLoop
  lifetime is no longer available
- an exception from one control callback is observed as
  `EventLoopCallbackSource::Control` and cannot skip other sources from the
  same round
- lifecycle attach capacity/allocation failure is reported before a participant
  is published and the participant must roll back owner-thread construction
- lifecycle `signal()` returns Accepted only after the node is linked dirty or
  found already dirty for the same generation; detach/quit cannot revoke that
  committed work
- a stale/detached lifecycle source returns OwnerUnavailable and cannot dirty a
  node that reused storage
- lifecycle callback exceptions follow the internal callback-exception policy,
  preserve later dirty nodes, and still permit detach reclamation
- Quiescing must not block waiting for completion packets; each pass uses
  zero-timeout backend polling and a bounded lifecycle drain, then repeats
  while either side reports progress or an outstanding obligation
- Shutdown with a non-silent lifecycle hub or retained IOCP completion
  obligation is a contract violation

v1 should prefer predictable behavior over over-complicated generic error models.

---

## 11. Future Extension Points
- coroutine awaiter scheduling
- metrics/tracing hooks
- idle strategy tuning
- task priority experimentation

These extensions must preserve EventLoop as the single-thread scheduling core.

---

## 12. Test Contracts
- same-thread runInLoop executes immediately
- cross-thread queueInLoop executes on loop thread
- cross-thread queueInLoop wakes blocked poll
- quit causes safe loop exit
- pending functors preserve expected execution semantics
- bounded admission rejects beyond the configured count without losing any
  already accepted functor
- a per-iteration batch limit yields back to ready timers/I/O before executing
  the remaining accepted batch
- an active batch larger than its configured budget remains loop-owned across
  rounds; a timer/control callback may remove and destroy an undispatched
  Channel, and the continuation skips its invalidated slot
- expired-timer and control populations larger than their configured budgets
  yield to later phases each round, preserve deterministic ready order, and
  publish drained/remaining/oldest-lag/exhausted observations
- quit still drains already-queued nested functors before loop exit
- executor identity is stable, cross-thread queueing executes on the owner
  thread, and copied handles become unavailable after loop teardown
- work accepted before executor admission closes can still perform owner-only
  operations during the final drain, while new submissions are rejected
- repeated `quit()` after loop exit does not resurrect executor availability or
  final-drain owner identity
- normal-plus-reserved saturation does not reject a registered control source
- 10,000 notifications for one source coalesce into one pending source
- registration cannot exceed `maxControlSources`
- notifying from inside a control callback schedules a later round without
  recursive callback execution
- one throwing control callback does not suppress later sources
- a notification racing with quit either returns Accepted and executes in the
  final drain, or returns Shutdown/OwnerUnavailable and does not partially run
- update/remove channel routes through correct Poller interaction path
- two synthetically captured active Channels exercise pending-peer removal and
  destruction identically on Linux and Windows without depending on IOCP batch
  dequeue width
- the Windows Poller contract interleaves a wakeup with more than 64 synthetic
  completions, verifies bounded two-round progress, exact completion-obligation
  release, distinct-Channel batching, and same-Channel deferral across
  registration-safe rounds
- the Windows wakeup-coalescing contract verifies a multi-producer burst emits
  one physical packet, producers on both sides of the owner reset cannot lose
  accepted work, self-rearm remains non-recursive, and quit/final-drain
  consumes the last physical packet before Shutdown
- current-Channel internal retirement remains available when normal and
  reserved pending-functor capacity are both full
- lifecycle attach/detach generation invalidates stale handles and prevents
  ABA when node storage is reused
- a signal committed before detach executes exactly once-or-coalesced before
  that generation is reclaimed; a signal linearized after detach returns
  OwnerUnavailable
- normal-plus-reserved saturation cannot reject a lifecycle signal
- more dirty lifecycle nodes than one drain budget are processed across
  self-rescheduled rounds without starving a ready Channel or timer
- callback self-signal is non-recursive and callback detach is reclaimed only
  after its active frame returns
- quit racing committed lifecycle work either drains Accepted work or rejects
  it before commitment; no accepted generation is stranded
- Windows cancel plus quit continues polling until the real
  `ERROR_OPERATION_ABORTED` completion is consumed and the lifecycle node
  becomes silent
- Windows AcceptEx/ConnectEx stop or destruction in the same owner callback as
  quit drains the real completion packet and releases all backend/owner leases
  before Shutdown
- `tests/contract/event_loop/test_event_loop_lifecycle_hub.cpp` verifies
  committed-notify, intrusive dirty coalescing, detach-generation ABA
  protection, saturation isolation, budgeted self-reschedule, callback
  re-entry, and quit linearization
- `tests/integration/tcp/test_iocp_quit_completion_drain.cpp` verifies the
  Windows cancel/quit completion-drain fixed point
- `tests/integration/tcp/test_iocp_accept_connect_quit_completion_drain.cpp`
  verifies that fixed point for Acceptor and Connector
- `tests/contract/event_loop/test_event_loop_control_saturation.cpp` verifies
  bounded registration, saturation isolation, coalescing, non-recursive
  re-arming, callback-exception containment, and quit linearization
- `tests/contract/event_loop/test_event_loop_wakeup_coalescing.cpp` verifies
  logical-versus-physical wakeup accounting, reset races, self-rearm, and
  final-drain convergence on Windows
- `tests/contract/event_loop/test_event_loop.cpp` verifies
  Channel, Timer, pending-functor, and metric callback exceptions are observed,
  later callbacks still run under Continue, Quit still drains accepted work,
  and a throwing thread-init callback is rethrown to `startLoop()` without a
  worker `terminate` or publication deadlock

---

## 13. Review Checklist
- Does this change violate single-owner-thread discipline?
- Is wakeup still correct and necessary?
- Are pending functors executed in a predictable place?
- Does this complicate future timer/coroutine integration?
- Is destruction/shutdown path still safe?
