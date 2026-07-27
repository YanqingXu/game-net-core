# thread_affinity_rules.md

## 1. Core Principle
game-net-core follows a reactor-style thread-affinity model.
Mutable reactor state belongs to a specific EventLoop thread.

## 2. EventLoop
- Each EventLoop has exactly one owner thread
- loop() must run only on the owner thread
- Poller operations must run on the owner thread
- pending functors are executed on the owner thread

## 3. Allowed Cross-Thread Interaction
Cross-thread interaction must go through:
- runInLoop(...)
- queueInLoop(...)
- typed EventLoopExecutor/control-source admission
- generation-tagged EventLoop lifecycle signal
- wakeup mechanism

No other direct mutation path is allowed for core loop state.

## 4. runInLoop
- If current thread == owner thread: execute immediately
- Else: enqueue and wakeup loop if needed

## 5. queueInLoop
- Enqueue only within the configured normal-plus-reserved hard capacity;
  saturation is explicit and never silently drops accepted work
- Capacity-aware callers use `tryQueueInLoop()` / `EventLoopExecutor::tryQueue()`
  and handle a false result
- Must ensure wakeup when loop may be blocked in poll
- Its reserve is not a lifecycle progress guarantee and must not be used by new
  internal control paths

## 5.1 EventLoop Control Sources
- Registration and unregistration are EventLoop owner-thread-only
- Registration access is source-private and must not be exported as an
  installed public scheduling API
- Registration happens before `loop()` and is bounded by
  `EventLoopOptions::maxControlSources`
- `EventLoopControlSource::notify()` may be called from any thread
- Each registered source owns one preallocated pending bit; repeated notify
  calls coalesce and allocate no queue node
- Control callbacks execute only on the owner thread, after active Channel and
  timer dispatch and before normal pending functors
- A source executes at most once per control round; self-notify is deferred
- Quit linearizes external control admission: Accepted work is final-drained,
  later external work returns Shutdown, and expired targets return
  OwnerUnavailable
- Final-drain completion and control-plane Shutdown are published together;
  later `quit()` calls cannot set executor owner-drain identity back to true
- During Draining, only the currently executing control callback may re-arm
  its own source; unrelated owner-thread work has no control-admission bypass
- User callbacks and business/data work are forbidden from using control
  sources merely to bypass queue admission

## 5.2 EventLoop Lifecycle Hub
- Lifecycle attach/detach and callback execution are EventLoop owner-thread-only
- `EventLoopLifecycleSource::signal()` may originate on any thread
- Signal performs no allocation: under the hub synchronization boundary it
  generation-checks the node and links its embedded dirty entry at most once
- Accepted signal is committed before return and must execute before that
  generation can be reclaimed; detach invalidates later signals but cannot
  discard an earlier committed signal
- Dirty callbacks are budgeted. Remainder/self-signal requests a later loop
  turn through wakeup and never consumes pending-functor capacity
- Quit transitions Running to Quiescing and seals new external lifecycle
  admission. The owner continues lifecycle drain and zero-timeout backend poll
  until both the hub and completion backend are silent
- FinalDraining runs already-accepted functors and committed internal work to a
  fixed point; only then may EventLoop publish Shutdown
- User/data callbacks may not use lifecycle nodes as a priority queue

## 6. Channel
- Channel update/remove must occur on its owning EventLoop thread
- handleEvent executes in EventLoop thread unless explicitly documented otherwise
- active-batch epoch/index assignment and invalidation are owner-thread-only
- successful remove invalidates the Channel's current batch slot before the
  caller may release ownership; dispatch never dereferences an invalidated slot
- remove/re-register changes the registration generation and stops remaining
  callbacks from the old readiness snapshot
- source-private retirement of the currently executing removed Channel is an
  inline owner-loop operation and never uses pending-functor admission

## 6.1 Connector and TcpClient
- Connector mutation, retry/timeout handling, Channel cleanup, and connected-fd
  publication are owner-loop-only
- Connector `state()` and `retryEnabled()` are atomic diagnostic snapshots;
  they do not grant cross-thread mutation rights
- Connector callbacks execute on the owner loop and may re-enter its lifecycle
  methods; code continuing after a callback must revalidate request generation
  and may not unconditionally overwrite a state published by that re-entry
- TcpClient is the cross-thread facade. Non-owner lifecycle calls use typed
  executor admission and owner-thread calls execute inline only while executor
  admission is still accepting
- TcpClient publishes a latest-accepted generation under its admission mutex,
  releases that mutex, and only then executes Connector, TcpConnection, or
  user callback code
- A rejected facade call cannot advance the published generation and cannot
  stale an earlier Accepted operation
- An accepted explicit connect observed while the current TcpConnection is
  already disconnecting is owner-loop state, separate from automatic retry,
  and survives that old connection's remove callback
- TcpClientControl calls mutate only their shared generation-tagged mailbox
  and signal one lifecycle node; they never directly read TcpClient fields
- The control lifecycle callback snapshots the latest request, releases the
  mailbox lock, and invokes TcpClient only on the owner loop while the mailbox
  still publishes a live target
- TcpClient closes the mailbox and detaches the lifecycle node on the owner
  loop before destruction, so stale handles return OwnerUnavailable

## 7. TcpConnection
- State transitions occur on owning EventLoop thread
- `connected()` and `disconnected()` are cross-thread-safe snapshot observers;
  no other loop-owned state becomes cross-thread-readable through them
- `droppedNotificationCount()` is a cross-thread-safe atomic diagnostic
  snapshot and does not expose other loop-owned state
- Actual socket write/read handling occurs on owning EventLoop thread
- Cross-thread send request must be marshaled back into loop thread
- Socket-option mutation, context access, callback replacement, connection
  establishment, and connection destruction are owning-EventLoop-thread only
- Mandatory output-buffer, backpressure, write-interest, and disconnecting
  half-close transitions occur before optional high-water/write-complete
  callback submission
- Optional high-water/write-complete callbacks use capacity-aware normal-queue
  admission; rejection is counted and cannot throw across the mandatory state
  transition
- Windows low-water read resume executes inline on the owner loop and cannot
  depend on ordinary pending-functor capacity
- because that resume may invoke a re-entrant message callback, its caller must
  recheck connection teardown and pending-overlapped-write state before
  continuing the outer write transition
- Windows overlapped read/write submission and completion consumption execute
  on the connection owner loop. A synchronous non-pending submit error
  converges immediately through the owner-loop error/close path and must not be
  converted into a fabricated Channel event
- TcpConnection socket close, completion-obligation consumption, lifecycle
  detach, Channel removal, and disconnected publication are owner-loop-only
- cross-thread terminal requests signal the lifecycle node and do not depend on
  normal/reserved functor capacity

## 8. Logger
- Logger is process-global and is not owned by an EventLoop
- Log emission and configuration may be called from any thread
- Callback configuration is snapshotted under Logger-internal synchronization
- Output callbacks run without the configuration lock and may be concurrent
- Callback-owned sinks and captured mutable state require their own synchronization
- `resetForTesting()` requires a quiescent test boundary

## 9. TcpServer Shutdown
- Graceful-stop requests may originate on any thread
- Acceptor state, connection-map state, drain timers, and terminal decisions
  are mutated only on the server base loop
- Per-connection shutdown and force-close remain marshaled to each connection's
  owner loop
- Completion is published only after worker-loop join has converged
- Base-loop stop admission and each worker aggregate use lifecycle sources, so
  the number of stop-control signals is bounded by workers rather than
  connections
- Worker cleanup signals base bookkeeping without blocking; base publishes
  generation-tagged BaseReleased only after map/admission release; worker ack
  follows final Channel/callback cleanup
- Thread-pool quit/join is forbidden before every current worker generation
  has acked

## 10. Listener Error Policy
- Acceptor and TcpServer accept-error policy callbacks execute on the base /
  Acceptor owner EventLoop
- Runtime accept retry timers and Channel read-interest changes remain confined
  to that same owner loop
- A policy callback may choose Retry or Stop; neither action permits direct
  worker-loop mutation

## 11. Callback Exceptions
- EventLoop exception handlers execute on the affected EventLoop owner thread
- TcpConnection exception observers execute on that connection's owner loop
- Asynchronous callback exceptions never migrate cleanup to another thread;
  Continue/Quit and connection close actions reuse normal owner-loop paths
- Thread-init exceptions cross back to the creator only through the synchronized
  EventLoopThread startup handshake

## 12. Wakeup
- Wakeup write can be invoked cross-thread
- Wakeup read/clear is handled in loop thread
- Wakeup is a scheduling signal, not business event delivery

## 13. TcpServer Admission
- Global/per-peer active counts, per-peer rate buckets, peer-table expiry, and
  unauthenticated timers belong to the server base EventLoop
- Worker-loop authentication completion is a request marshaled through the
  base-loop executor; base-loop execution order resolves deadline races
- Admission metric callbacks execute on the base loop and may not move socket
  or connection lifecycle work onto an arbitrary caller thread

## 14. Metrics Export
- MetricsExporter has no owner EventLoop and may receive concurrent calls from
  multiple producer loops
- recorder callbacks execute synchronously on the original hook thread and do
  not move producer work to another thread
- InMemoryMetricsExporter synchronizes only its own aggregate maps; it never
  mutates reactor or game state
- snapshot rendering and any future blocking export I/O must run outside hot
  owner-loop callbacks
- recorder adapters contain exporter exceptions at the observability boundary

## 15. Forbidden
- Direct Poller mutation from non-owner thread
- Direct Channel mutation that changes registration from non-owner thread
- User callback execution in ambiguous thread context
- An unbounded emergency/control queue
- Treating the public `queueInLoop()` reserve as guaranteed lifecycle capacity
- Executing Connector, TcpConnection, or user callback code while holding the
  TcpClient admission mutex
- Waiting for an IOCP completion after a synchronous submit failure that
  established no kernel-owned completion obligation
- Deferring current-Channel destruction through the normal or reserved pending
  functor queue
- Blocking an EventLoop owner thread while waiting for BaseReleased, worker
  ack, or an IOCP completion
- Publishing EventLoop Shutdown while the lifecycle hub or IOCP retained
  completion set is non-silent

## 16. Fault and Endurance Drivers
- fault clients may own and operate their own raw sockets but must not mutate
  TcpServer or TcpConnection loop-owned state directly
- injected callback, overload, reset, and shutdown paths retain the existing
  base-loop and connection-loop callback affinity
- the endurance supervisor owns a child process and evidence files only; it has
  no access path to Reactor mutable state
- “Occasionally safe” thread behavior without rule support
