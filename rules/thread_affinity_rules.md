# thread_affinity_rules.md

## 1. Core Principle
game-net-core follows a reactor-style thread-affinity model.
Mutable reactor state belongs to a specific EventLoop thread.

## 2. EventLoop
- Each EventLoop has exactly one owner thread
- loop() must run only on the owner thread
- Poller operations must run on the owner thread
- pending functors are executed on the owner thread
- Windows completion batches are dequeued, translated, merged, and dispatched
  only on that EventLoop owner thread
- the IOCP dequeue width is validated in `[1, 64]` before Poller construction;
  changing the width never moves completion or Channel state off the owner loop
- Windows AcceptEx/ConnectEx submission, cancellation, Channel-observer
  revocation, and completion-obligation tracking are owner-thread-only; final
  drain performs only zero-timeout owner-thread polls
- every Windows AcceptEx slot is created, submitted, completed, generation-
  advanced, canceled, and reused only by the Acceptor base loop; Poller may
  publish the completed operation identity but never mutates slot policy
- Poller may append multiple completed Accept identities to the listen
  Channel's owner-loop queue in one batch; only the subsequent Acceptor
  callback may drain slots or replenish the pool

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
- Partial active-Channel batches, ready timers beyond the current budget, and
  undrained control mailbox bits remain owner-loop/source-owned; continuation
  never transfers their callback targets through the pending-functor queue
- Every fairness budget is validated before `loop()` starts. Budget exhaustion
  causes an immediate/non-blocking next turn while timer, control, lifecycle,
  and accepted-functor phases continue between portions
- Fixed-rate timer catch-up remains normal owner-loop timer work: each replay is
  reinserted through TimerQueue, consumes a later timer budget slot, and never
  invokes the callback recursively. Once its configured consecutive catch-up
  count is exhausted, TimerQueue advances directly to a future cadence point
- DeadlineQueue is owner-loop-only. A driver may be one TimerQueue timer, but
  logical targets never receive one timer callback each; expiration policy runs
  after a budgeted advance returns generation-tagged values to the owner

## 6. Channel
- Channel update/remove must occur on its owning EventLoop thread
- handleEvent executes in EventLoop thread unless explicitly documented otherwise
- active-batch epoch/index assignment and invalidation are owner-thread-only
- successful remove invalidates the Channel's current batch slot before the
  caller may release ownership; dispatch never dereferences an invalidated slot
- slot invalidation remains mandatory between budgeted dispatch portions, when
  removal originates from a timer, control, lifecycle, or functor phase
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
- output-memory admission before marshaling may update only the connection's
  fixed atomic connection/loop/server/global reservation chain. It invokes no
  callbacks and grants no cross-thread access to Buffer, Channel, segment, or
  server-map state
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
- Windows output-segment enqueue, front-offset advancement, segment removal,
  chunk submission, and close-time discard are connection owner-loop-only.
  Cross-thread send owns only its admitted immutable payload until the
  executor moves it into that queue
- Windows read-storage allocation, bounded reuse, submission, and final
  release are connection owner-loop-only. A pending `WSARecv` keeps the same
  allocation until its real normal/error/cancellation completion is consumed
- transport fixed-storage accounting updates occur only at owner-controlled
  allocation/construction/release/destruction boundaries. The public
  process-level snapshot is cross-thread-safe, approximate across concurrently
  changing categories, and grants no access to owner-loop objects
- Buffer retention growth, arm state, trim, and snapshots are owner-loop-only;
  no retention observation becomes cross-thread-safe merely because Buffer is
  embedded in TcpConnection
- `TcpConnection::memoryRetentionSnapshot()` is likewise owner-loop-only.
  Capacity profiles spanning worker loops post one sampling task through each
  endpoint executor and aggregate returned values off-loop; they never read
  Buffer or transport fields directly from the management thread
- TcpConnection socket close, completion-obligation consumption, lifecycle
  detach, Channel removal, and disconnected publication are owner-loop-only
- on Linux, TcpConnection disables and removes its Channel registration before
  explicitly closing the socket, so the released numeric fd cannot collide
  with a stale Poller entry during callback-driven reconnect
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
- EventLoopThreadPool policy changes, loop selection, and connection-load
  accounting are base-loop-only. Queue-lag selection may lock each worker's
  pending-functor queue for a snapshot but never reads worker-owned Channel,
  timer, connection, or completion state
- selector decisions never migrate an established connection; the selected
  loop owns it for its full TcpConnection lifetime
- TcpServer constructs and associates per-loop/server output budgets before
  accepting connections. Low-frequency snapshot aggregation may lock only the
  budget-container metadata; no send acquires that lock or a process-global
  mutex
- TcpServer establishment is admitted through the selected worker's bounded
  normal functor queue. Before base-thread construction of the worker-owned
  TcpConnection, one worker lifecycle rollback obligation must already be
  Accepted; this obligation carries cleanup only and is not an establishment
  priority lane
- QueueFull, Shutdown, OwnerUnavailable, or a later setup exception rolls back
  base-map/load/admission state on the base loop, then calls connectDestroyed
  and releases the last unestablished TcpConnection reference on its selected
  owner loop. TcpConnection construction, establishment, destruction, and
  final shared-owner release may not escape this transaction
- A worker lifecycle callback that observes an establishment transaction still
  being resolved re-arms only itself. This accepted obligation participates in
  final drain, so worker quit/join cannot overtake the base decision
- the worker rollback registry is capped at its normal pending-functor
  capacity. Reaching that cap rejects the raw accepted fd before
  TcpConnection construction and does not expand lifecycle-lane storage

## 10. Listener Error Policy
- Acceptor and TcpServer accept-error policy callbacks execute on the base /
  Acceptor owner EventLoop
- Runtime accept retry timers and Channel read-interest changes remain confined
  to that same owner loop
- A policy callback may choose Retry or Stop; neither action permits direct
  worker-loop mutation
- Windows Retry cancels the whole submitted AcceptEx generation and may
  replenish the fixed pool only after every cancellation completion has been
  consumed and the retry delay has elapsed
- accepted-socket callbacks may re-enter `stop()`; code after callback entry
  must not assume that the listener or any newly replenished slot remains live

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
- An IOCP wakeup packet may share a dequeue batch with real I/O; consuming the
  signal must not stop translation of the remaining completion entries
- IOCP producers may only change the Poller-owned atomic pending bit and post
  the packet won by a false-to-true transition; only the EventLoop owner may
  clear that bit after dequeue
- A producer ordered before the owner clear merges into the current awake
  iteration; a producer ordered after the clear must post the next packet.
  Tests must cover both orderings so reset cannot strand accepted work
- Scheduling handles hold their admission-state synchronization through the
  wakeup call. EventLoop destruction closes those states before destroying the
  Poller, so an admitted producer cannot race a destroyed completion port

## 13. TcpServer Admission
- Global/per-peer active counts, per-peer rate buckets, peer-table expiry, and
  the authentication DeadlineQueue/driver belong to the server base EventLoop
- Worker-loop authentication completion is a request marshaled through the
  base-loop executor; base-loop execution order resolves deadline races
- A driver advance extracts only its configured budget. If ready deadlines
  remain, one base-loop continuation advances another batch; no per-connection
  callback is submitted to a worker or the general functor queue
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

## 14.1 Phase 4/5 Game Handoffs
- SessionManager/player/session indexes remain management-loop-owned; its
  cross-thread posts return typed admission and accepted posts publish one
  terminal result
- SessionBinding is an immutable cross-thread validation capability; its atomic
  current-generation check grants no mutation or owner-loop access
- LogicLoop validates a tracked generation on its owner immediately before
  handler and output callbacks
- Pipeline I/O, management, logic and endpoint handoffs must inspect typed
  admission. A rejected data handoff may request terminal endpoint close through
  the endpoint control-safe close path but may not mutate endpoint state
  off-thread
- Broadcast routing is management-loop-only. Dispatcher outstanding admission
  may be concurrent but mutates only the fixed atomic owner-task ->
  owner-byte -> global-byte reservation chain; endpoint sends still execute
  only on their endpoint owner loops
- Broadcast owner identities are first-observation registry metadata. Their
  immutable registry publication may take a registration mutex, but repeated
  reservation, rollback, completion release, shutdown, and snapshot paths do
  not acquire it
- PacketFramer retention growth, trim, reset, and snapshots remain on its
  single caller-owner thread and never schedule their own continuation

## 15. Forbidden
- Direct Poller mutation from non-owner thread
- Direct Channel mutation that changes registration from non-owner thread
- User callback execution in ambiguous thread context
- An unbounded emergency/control queue
- Treating the public `queueInLoop()` reserve as guaranteed lifecycle capacity
- Using the lifecycle lane to run successful connection establishment or user
  callbacks; its establishment role is limited to rollback of a transaction
  whose normal queue admission failed
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
- Ignoring a Phase 4/5 handoff result or leaving an Authenticating/Online
  connection live after the handoff required for progress was rejected

## 16. Fault and Endurance Drivers
- fault clients may own and operate their own raw sockets but must not mutate
  TcpServer or TcpConnection loop-owned state directly
- injected callback, overload, reset, and shutdown paths retain the existing
  base-loop and connection-loop callback affinity
- the endurance supervisor owns a child process and evidence files only; it has
  no access path to Reactor mutable state
- “Occasionally safe” thread behavior without rule support
