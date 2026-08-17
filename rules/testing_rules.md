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
- repeating timers prove legacy fixed-delay ordering and deterministic
  fixed-rate cadence with an exact maximum catch-up count followed by skip
- bucketed deadlines prove no-early expiration, generation-safe replacement,
  exact budget continuation, large-future-population isolation, clear, and
  owner-thread enforcement
- EventLoopThreadPool contracts cover round-robin, exact least-connection load
  commit/release, pending-functor queue-lag preference with rotating ties, and
  deterministic non-empty-key consistent hashing
- EventLoopThreadPool negative contracts cover non-negative thread counts,
  base-loop-only configuration, immutable Started configuration, repeated-start
  rejection without duplicate workers, zero-thread initialization, partial-
  start rollback, and legal stop/restart with unchanged load accounting
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
- an authentication-deadline population larger than the configured advance
  budget drains through bounded base-loop continuations with exact accounting
- a session-idle population larger than the configured advance budget expires
  without a player-index scan and preserves generation replacement
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
- TcpServer selected-worker establishment with normal and reserved functor
  capacity saturated: no Accepted metric or connection callback, base
  map/load/admission and authentication deadline rollback to zero, accepted fd
  closes once, connectDestroyed plus final TcpConnection release occur on the
  worker, and a later healthy connection plus stop still converge
- TcpServer owner lifecycle-node capacity exhausted after establishment queue
  admission: accepted-socket accounting remains cumulative, connection and
  active-admission state roll back to zero without a connection callback, final
  release remains on the owner, and freeing capacity permits a healthy
  connection on the same server
- TcpServer injects a TcpConnection construction failure after every fallible
  member/callback setup step but before the connection Socket claims the fd;
  the hook observes no connection-side fd owner, the base guard closes the
  peer exactly once, no provisional/Accepted state escapes, and stop converges
- TcpClientControl admitted while the normal and reserved queues are full,
  latest-generation coalescing on the lifecycle lane, owner-loop execution,
  detach on destruction, and OwnerUnavailable from surviving handles
- TcpClient connected-fd receiver construction failure before socket claim:
  the local guard closes exactly once, active request is released before
  terminal notification, callback-reentrant reconnect is Accepted, and old
  Connector settlement cannot overwrite the replacement attempt
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
- concurrent Broadcast reservations never overshoot owner task/byte or global
  byte limits; later-scope rejection rolls back earlier scopes, shutdown races
  choose one admission side, stable snapshots report peak/rejection evidence,
  and all scopes converge to zero
- hierarchical TCP output admission rejects at the exact connection, loop,
  server, or optional shared-global scope; concurrent reservations never
  overshoot, later-scope failure rolls back earlier scopes, hysteretic recovery
  reopens admission only at the configured threshold, and close/stop leaves
  every snapshot at zero
- Buffer and PacketFramer retained-capacity contracts cross the configured high
  target, remain untrimmed above the lower recovery threshold, preserve every
  unread/wrapped byte through trim, converge at or below the target, and reuse
  that capacity without per-small-read/frame retrimming
- fixed-storage retention contracts use the public process snapshot to prove
  shared read-pool/slab bytes remain zero, one IOCP Poller contributes one
  finite fixed batch workspace, one AcceptEx pool contributes slot bytes until
  its final shared lease is released, one connection contributes no bytes
  before the first read and at most 4 KiB afterward, category/total peaks are
  monotonic, and every current category returns to zero after teardown
- the slow-broadcast-recovery capacity profile must use real TCP endpoints,
  hold reads during pressure, keep aggregate connection pending bytes within
  the configured connection hard-limit sum, keep dispatcher outstanding bytes
  within its global limit, account every terminal endpoint as accepted or one
  typed reason, reconcile EndpointOverloaded with the TCP rejecting scopes,
  and remain below the recovery threshold for a configured stable window
  before reporting recovery
- the scale-ready mixed capacity profile must recover slow clients through a
  fixed-size nonblocking reader pool with stable disjoint socket ownership;
  exact worker/assigned/closed counts must converge before teardown; candidate
  and dedicated runs must record the same reviewed finite server send-buffer
  request so Linux and Windows typed overload is independent of OS defaults;
  and 10k candidate versus 100k dedicated endpoint-attempt parameter drift
  must be rejected by structured evidence guards
- a production promotion artifact must revalidate retained raw capacity and
  endurance evidence rather than trusting a copied summary; candidate mode
  requires 10k plus 24h, release mode requires dedicated 100k plus same-SHA
  24h/72h, and SHA, workflow run, rerun attempt, stage, profile, duration, hash,
  or source-inventory drift must fail closed

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
