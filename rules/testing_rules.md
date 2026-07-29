# testing_rules.md

## 1. Testing Philosophy
Tests are not only correctness checks.
They are executable contracts.

## 2. Required Test Categories
Each core module should have:
- unit test
- contract test
- failure-path test
- threading-related test if cross-thread behavior exists

## 2.1 Test Layering
Tests should live in:
- `tests/unit/<module>/`
- `tests/contract/<module>/`
- `tests/integration/<module>/`

Layer meaning:
- unit = local logic and small invariants
- contract = public API, lifecycle, thread-affinity, callback ordering
- integration = end-to-end main path validation across modules

## 3. Unit Test Focus
Unit tests verify:
- local logic
- state transitions
- boundary behavior
- small invariants

## 4. Contract Test Focus
Contract tests verify:
- public API guarantees
- module interaction promises
- lifecycle constraints
- thread-affinity behavior
- callback ordering where relevant

## 5. EventLoop Required Test Examples
- runInLoop executes immediately on same thread
- queueInLoop executes later on loop thread
- cross-thread queueInLoop wakes blocked loop
- quit exits loop safely
- normal-plus-reserved saturation still admits registered control notifications
- control-source registration capacity is finite
- repeated same-source notifications coalesce and self-notify is non-recursive
- control callback exceptions do not suppress other sources
- notify-versus-quit races prove Accepted work is drained and rejected work
  returns Shutdown/OwnerUnavailable
- dynamic lifecycle attach/detach with committed-notify linearization
- dirty-set coalescing, generation/ABA rejection, callback self-signal, and
  callback-frame-safe reclamation
- a dirty population larger than the lifecycle budget self-reschedules while
  ready I/O/timers retain service
- an active batch larger than its budget survives across owner-loop rounds,
  and an intervening phase destroys an undispatched Channel only after its
  indexed slot is invalidated
- expired-timer and control populations larger than their budgets yield to
  accepted functors and publish exact drained/remaining plus exhausted metrics
- one deterministic sustained-source contract keeps active I/O, timer,
  control, lifecycle, and functor work pending together for multiple rounds;
  every source advances once per round in documented order with callback depth
  one
- Windows cancel-versus-quit consumes the real IOCP completion before Shutdown
- Windows AcceptEx/ConnectEx cancellation in the same owner-loop callback as
  destruction and quit releases outstanding, retained-storage, Channel, and
  owner-guard counts before Shutdown
- Windows multi-producer wakeup bursts distinguish logical notifications from
  physical completion packets and prove one packet while pending
- a configured IOCP dequeue width below 64 must bound each kernel poll while
  preserving exact Accept/read/write identities and obligations across rounds
- deterministic hooks place a producer immediately before and after the owner
  clears IOCP wakeup-pending; accepted work must execute in both cases
- Windows wakeup self-rearm plus quit must drain accepted work and leave no
  pending physical packet before Shutdown

## 6. Channel Required Test Examples
- handleEvent dispatches correct callback by revents
- tied owner expired => dangerous callback path blocked
- update/remove workflow respects loop-thread contract
- deterministic two-entry active batch where the first callback removes and
  destroys the not-yet-dispatched tied owner
- current callback self-remove and owner release
- same-fd replacement survives an explicitly rejected stale repeated remove
- combined readiness stops after remove/re-register changes the registration
  generation

## 7. Poller Required Test Examples
- updateChannel registers correctly
- removeChannel unregisters correctly
- poll returns active channels accurately
- invalid removal path is detected or guarded
- Windows bounded batch dequeue with more than 64 completion packets, an
  interleaved wakeup, exact outstanding-operation release, distinct-Channel
  batching, same-Channel read/write deferral across poll rounds, and deferred
  error preservation after the first callback removes the Channel
- Windows wakeup coalescing remains allocation-free, does not truncate the
  real-I/O entries beside its packet, and exposes exact physical post/consume
  counts only through a source-private repository-test seam
- Windows association-preserve and replacement-registration fault injection
  both prove transactional TcpClient rollback and successful fresh reconnect

## 8. Lifecycle-Sensitive Modules
For lifecycle-sensitive modules, tests should include:
- remove-before-destroy
- callback-after-destroy prevention
- repeated close/error handling guard
- registration state consistency
- active-batch invalidation before pending-peer destruction
- current-Channel retirement under normal-plus-reserved queue saturation
- finite-limit rejection and accounting release after accepted connection close
- timeout-versus-success races for deadline-based admission
- bounded control-plane saturation and final-drain races
- normal-plus-reserved queue saturation while TcpConnection applies
  backpressure/write-interest before dropping optional notifications, followed
  by output-reservation and disconnecting-half-close convergence
- deterministic Windows `WSAENOBUFS` / `WSAECONNRESET` submit failures and a
  real `ERROR_OPERATION_ABORTED` cancellation completion, proving immediate
  error handling, no phantom pending operation, and single-shot close
- deterministic Windows write-chunk injection proves each physical `WSASend`
  is bounded to `ULONG`, a multi-segment queue preserves payload order, and one
  large segment advances its front offset across partial completions without a
  complete-suffix copy
- Windows segmented-write tests compare peak queued segment bytes with
  `pendingOutputBytes`, require exact zero after drain/close, and keep existing
  slow-peer, synchronous-failure, cancellation, and write-complete ordering
  contracts green
- deterministic Windows read-storage hooks prove zero allocation before first
  post, a 4 KiB submission/retention ceiling, reuse across multi-chunk input,
  and zero retained bytes only after pending-read cancellation is consumed.
  The Release connection benchmark must also accept a structured 10k idle
  profile so bytes-per-connection can be compared with the frozen baseline
- Connector owner-affinity rejection, callback re-entry, configured-backoff
  restart, and accepted/rejected facade-generation races, including two
  Accepted operations where the latest request supersedes an in-flight attempt
- AcceptEx and ConnectEx immediate-quit tests must first prove a real successful
  pending submission, then prove cancellation creates exactly one obligation;
  synchronous non-pending failures must create none
- Windows AcceptEx pool tests must prove default/configured finite depth,
  independent slot generations and socket ownership, allocation-free
  same-Channel Accept completion coalescing with exact identities,
  replenishment before burst callbacks serialize the pool, callback `stop()`
  re-entry, generation-wide Retry after a deterministic synchronous submission
  failure, and delayed cancellation consumption
- after multi-slot stop/destroy plus immediate quit, fixed-pool slot, slot-owned
  socket, retained completion, and shutdown-obligation counts must all be zero;
  the test seam must also prove pool capacity never exceeds its configured
  bound
- TcpConnection explicit socket close and completion-drain terminal state,
  including first-close-reason-wins and native error preservation
- Linux TcpConnection close-callback re-entry with deterministic numeric-fd
  reuse, proving the old Channel was removed before close publication and a
  repeated connectDestroyed cannot erase the replacement registration
- TcpServer base and all worker queues saturated while one aggregate stop
  signal per worker converges through BaseReleased, worker ack, join, and
  future completion
- TcpClientControl admitted while the normal and reserved queues are full,
  latest-generation coalescing on the lifecycle lane, owner-loop execution,
  detach on destruction, and OwnerUnavailable from surviving handles
- TcpServer and TcpClient observers receive the exact immutable
  TcpConnectionCloseInfo published before removal/reconnect decisions
- SessionManager post admission is saturated and shutdown-raced with exact
  QueueFull/Shutdown/OwnerUnavailable and terminal callback assertions
- duplicate-login generation rollover is forced after command admission but
  before Logic handler/output, proving no stale business side effect
- Pipeline framing continuation, I/O-to-management, auth, logic output and
  endpoint dispatch rejection each converge to close
- consecutive Broadcast plans cannot bypass per-owner/global outstanding
  budgets; every reservation is released on queue rejection, endpoint terminal
  result and callback exception

## 9. AI-Specific Requirement
When generating code, generate tests in the same change set.
No public interface should be added without at least one direct contract assertion.

## 9.1 Change Gate Requirement
For core modules, the change description must name the specific test file that validates the behavior.
"covered by tests" is not sufficient.

For lifecycle-hub changes, the named minimum evidence is:
- `tests/contract/event_loop/test_event_loop_lifecycle_hub.cpp`
- `tests/integration/tcp/test_iocp_quit_completion_drain.cpp`
- `tests/integration/tcp/test_iocp_accept_connect_quit_completion_drain.cpp`
- `tests/contract/tcp_connection/test_tcp_connection_completion_drain.cpp`
- `tests/contract/tcp_server/test_tcp_server_saturation_shutdown.cpp`

Repository text guards may enforce presence/registration but cannot substitute
for executing these runtime contracts on Linux and Windows.

## 10. Forbidden
- test only happy path for lifecycle-heavy module
- treat coverage as substitute for contract quality
- no-thread test for cross-thread API

## 11. Production Endurance
- a normal CTest cycle must exercise every declared fault profile on Linux and
  Windows before a long-duration claim is eligible
- production duration evidence must come from one uninterrupted process with
  monotonic heartbeats, immutable commit identity, executable/log hashes, and
  independently observed wall time
- the child must remain alive until the supervisor acknowledges each heartbeat,
  so Linux RSS evidence is sampled from that exact live process after the
  corresponding cycle
- shortened smoke runs and combined shards are orchestration evidence only and
  cannot substitute for the fixed 24-hour or 72-hour gate
