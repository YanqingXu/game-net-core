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
- the source-private Engine contract rejects foreign-thread mutation, reports
  Running/Quiescing admission distinctly, and preserves cross-thread wakeup as
  its only direct producer operation
- a committed completion submission plus cancellation retains its lease until
  an actual terminal packet is drained during quit; a backend without
  Completion capability returns RejectedUnsupported without acquiring a lease
- the native Completion Engine contract proves nonzero operation generation,
  one terminal notice per Accepted generation, duplicate/rejected packet
  filtering, terminal status, owner-side transport bookkeeping, observer
  revocation, batch-owned lease retirement, direct EventLoop consumption of all
  four Accept/Connect/Read/Write kinds without fake Channel callbacks, one-budget
  continuation, and consumer-side terminal retirement even after observer
  generation replacement
- the Poller contract must exercise Windows completion progress through the
  production Engine wait/dispatch seam, prove same-Channel notices remain
  distinct, and assert zero fake Channel callbacks; source guards reject every
  legacy publisher, intrusive operation link, and Channel storage implementation
- the lifecycle-hub contract must observe the exact
  Running/Quiescing/FinalDraining/Shutdown order and re-enter `quit()` during
  FinalDraining to prove the public phase cannot rewind
- one budgeted Engine notice batch combines callback-local close, stale
  remove/re-register invalidation, callback exception containment, and a later
  continuation round with exact drained/remaining/exhausted metrics
- the legacy public IOCP dequeue option maps to source-private Engine backend
  capacity while `maxActiveChannelsPerIteration` remains the independent
  EventLoop dispatch budget
- the epoll Readiness Engine contract must prove nonzero registration identity,
  stable generation across interest disable/re-enable, a new generation after
  remove/re-register, stale-token rejection, and exact-fd/target cancellation
- multiple native masks for one current generation merge into one typed notice,
  while a queued mask for an interest that has since been removed is filtered;
  a fixed wait capacity reports exhaustion and later waits expose every ready
  source without invoking a callback inside the port
- level-triggered readiness repeats until the application consumes the source;
  the port consumes no payload, and the application still observes EAGAIN after
  draining it
- Linux cross-thread Engine wakeup is owned and drained by the readiness port,
  emits no user notice, and preserves EventLoop's wakeup observation; every
  other direct port operation rejects a foreign thread
- the default-off IOE-X1 contract must use a real Linux io_uring fd and exercise
  one-shot Accept, Recv, and Send end to end; a model-only queue is insufficient
- the contract fills the finite native SQ before flush and observes a typed
  `SubmissionQueueFull` without an overflow allocation or fallback, then proves
  accepted work still completes after flush
- a pending Recv is canceled during quiesce. The internal ASYNC_CANCEL CQE and
  the target `ECANCELED` CQE are counted separately, the target emits exactly
  one Cancelled notice, its lease remains live until notice transfer, and
  Shutdown waits for all staged/active/cancel obligations
- static governance keeps epoll as the normal Linux Core path and rejects
  multishot, provided-buffer selection, registered/fixed files, zero-copy Send,
  SQPOLL, install/export, or a Windows experimental fallback in IOE-X1
- the IOE-X1 directional benchmark is opt-in, Linux-only, non-installed, and
  absent from CTest; each successful sample must account for exactly two
  accepted and terminal one-shot operations per round trip, zero fallback,
  zero residual active operation/notice/owned-byte state, ordered percentile
  output, and a drained shutdown
- the IOE-X2 contract must use the real ring descriptor as a borrowed Channel
  source and complete one Recv/Send chain through EventLoop without directly
  calling Engine wait from the test
- the same contract leaves a second Recv pending, calls EventLoop quit from a
  completion consumer, and proves the automatically committed lifecycle
  participant cancels and terminally consumes it before Pump stop and
  EventLoop Shutdown; its lease remains live through that consumer frame
- dispatch capacity and CQ capacity are configured independently. More ready
  terminal notices than one pump turn must continue through the lifecycle lane
  without recursive consumer entry, pending-functor use, or lost completion
- injected/transient drive or consumer failure must be observable and may
  converge only as drained-after-failure; no failure path may detach the ring
  Channel, release an operation lease, or publish a drained future while any
  Engine obligation or decoded notice remains
- the IOE-X3 contract must use a real established stream socket and prove the
  driver never has more than one Recv and one Send identity active. A message
  callback pauses re-entrantly, peer input remains unread without repost, and
  resume posts exactly one replacement after cancellation/terminal retirement
- the same contract fills finite send-byte and segment admission, observes the
  exact typed rejection without fallback, drains accepted segments in FIFO
  order, and verifies partial-send bookkeeping cannot underflow or reorder
- explicit close and EventLoop quit each leave a real Recv pending, consume its
  target cancellation terminal, retain the socket until Pump physical stop,
  publish one first close reason, invoke one close consumer in Stopped state,
  and converge active operations, notices, owned bytes, and pending send bytes
  to zero. Foreign-thread mutation and throwing message callbacks are rejected
  or contained without callback-after-stop
- the IOE-X4 contract must use two simultaneous real loopback TCP connections
  on one Hub/Pump. Both own one Recv, both make send progress, and Pump metrics
  prove one shared Engine rather than a ring/Channel per connection
- filling one route's byte/segment budgets must return its exact typed rejection
  while the neighbor still admits and drains FIFO output; exceeding the separate
  Hub byte budget rolls back only the rejected call and preserves both routes'
  exact pending-byte accounting
- closing the saturated route cancels only its identities. The neighbor must
  receive and send afterward. The retired slot is then reused with a new
  generation from a close-callback re-entry; the old identity is stale and can
  neither send, close, nor hit replacement callbacks
- a second scenario quits EventLoop with two pending Recvs and accepted sends.
  Both connection futures must publish `EventLoopQuiescing`, every socket and
  fixed operation route must retire, aggregate/per-route bytes reach zero, and
  the Hub future plus Pump summary precede EventLoop Shutdown. Foreign mutation,
  connection-capacity rejection, and rejected-fd ownership rollback are direct
  assertions
- the IOE-X5 capacity contract must simultaneously admit exactly 256 real TCP
  routes to one Hub, close and generation-replace exactly 64, and prove all 192
  retained routes receive and send after replacement has begun. Old identities
  must reject without touching replacements
- capacity evidence must reconcile 320 connection futures and socket closes,
  exact initial/replacement/retained callback counts, every accepted send byte,
  a 256 active-route high-water mark, zero active operation-route entries, zero
  aggregate pending bytes, and zero Engine active/ready/owned-byte residue
- focused soak repeats the capacity/churn contract under ASan/UBSan and TSan.
  Its opt-in Release benchmark emits validated structured evidence and remains
  outside CTest; directional epoll comparison cannot by itself promote a public
  selector or production adapter
- the IOE-X6 cross-backend contract must use two real loopback TCP connections:
  one production epoll `TcpConnection`, one Hub-backed semantic adapter. Both
  exceed the same connection-local hard limit, cross high water, stop reading,
  resume at low water, deliver peer input, and permit callback-reentrant send
  plus forced close on their one owner EventLoop
- both paths publish `ForcedShutdown` once, reach `Closed` with zero pending
  output, and reject post-close send. The adapter future must already be ready
  and its Hub socket closed when close observers run; Hub stop must leave zero
  routes, operations, notices, and owned bytes
- a separate observer-lifetime case destroys the adapter with a real Recv
  pending. No adapter callback may run afterward, while the retained future
  must still report forced terminal close and zero physical Hub residue
- the IOE-X7 graceful case must place real accepted output behind a constrained
  TCP send buffer, request graceful shutdown before peer drain, reject new send,
  and prove both production epoll and Adapter peers receive the complete payload
  followed by local EOF. Each peer then sends data after observing that EOF and
  half-closes; both owners must deliver the inbound data before terminal close
- Adapter metrics must report one graceful request and one native write
  half-close, zero discarded accepted bytes, one peer-EOF physical terminal,
  `GracefulShutdown` semantic close info, future-before-close-callback ordering,
  and zero Hub/Engine residue. Repeated shutdown plus force escalation must
  retain `GracefulShutdown` while cancelling/draining the remaining Recv once
- the IOE-X8 cross-thread contract must call production `TcpConnection` and the
  Adapter from non-owner threads over real loopback TCP. A send accepted before
  graceful shutdown must reach the peer before EOF; callbacks remain on the
  owner and terminal publication remains single-shot
- a finite Adapter mailbox fixture must deterministically fill without running
  the owner, return `SchedulingQueueFull`/`QueueFull` without retaining rejected
  work, then recover after a bounded drain. Concurrent graceful/force requests
  must retain the first admitted reason and reject later send
- observer-revocation and owner-quit cases must leave no pending command,
  reserved byte, route, operation, notice, socket, or owned-byte residue. Tests
  must join active facade calls before owner destruction and must prove no
  accepted command retains or invokes the dead observer
- the IOE-X9 listener contract must use real AF_INET loopback sockets and drive
  production epoll `TcpServer` plus the shared-Pump listener through the same
  bind/listen, client connect, echo, peer close, and owner stop observations.
  Both paths must invoke user callbacks on their owner and publish one terminal
- an Adapter/Hub fixture must arm a finite one-shot Accept window, accept a
  burst up to route capacity, reject and close overflow accepted fds, retire
  the active routes, then admit a replacement wave without stale generation
  delivery. Callback re-entry may stop the listener or Hub and must not rearm
- deterministic operation pressure must reject the first Accept arm with a
  typed result and a ready listener summary, closing the transferred listener
  once while existing routes remain valid. Explicit stop and EventLoop quit
  must cancel every exact Accept, publish listener-before-Hub stop, and leave
  zero active Accept/route/operation/notice/fd/byte state under normal,
  ASan/UBSan, TSan, and focused repeat gates
- the IOE-X10 listener decision fixes 256 concurrent active routes, Hub route
  capacity 256, `maxPendingAccepts == 32`, four churn waves replacing exactly
  64 routes, 100 measured echo round trips per active route and wave, and a
  64-byte payload. A runner or environment that cannot execute those values
  records `DEFER`; it must not lower them and report success
- production epoll and the source-private completion listener must execute the
  same bounded connection/churn/echo state machine in Release on one machine,
  compiler, build, client topology, and CPU-affinity set. Each backend receives
  one full unrecorded warm-up and five retained samples; formal pairs alternate
  which backend runs first and preserve the exact run order
- every X10 sample must validate connect/echo/close totals, completion rate,
  throughput, ordered P50/P99/P999 latency, fd/route/Accept/Recv/Send/pending-
  byte/Engine-owned-byte/RSS high-water observations, typed capacity and SQ
  rejection counts plus recovery, shutdown latency, and zero listener/route/
  operation/notice/fd/pending-byte/Engine-owned-byte residue. Epoll operation
  fields are explicitly unavailable and cannot be synthesized from readiness
- the X10 evidence manifest records CPU, kernel, compiler, build type, affinity,
  warm-up count, interleaving order, five hashes per backend, medians, and every
  validation check. Missing/invalid samples, correctness/accounting/recovery/
  lifecycle failure, nonzero residue, or metadata drift forces `DEFER`; only a
  complete valid set may record narrowly scoped `PROMOTE` for later source-
  private shaping, never a public selector or production replacement
- the ARCH-G1 Hub observation regression contract invokes `listening`,
  `listenerMetrics`, `phase`, and `metrics` from a real foreign thread and
  requires every call to reject before reading owner state; owner-side queries
  and shutdown must remain valid afterward
- the IOE-X11 contract must use a real loopback TCP listener created by the
  source-private Server. It proves the recorded ephemeral bind address accepts,
  each accepted socket settles into exactly one semantic Adapter, echo and
  connection callbacks remain on the owner, and callback-reentrant graceful
  stop drains accepted bytes through peer-observed EOF before terminal close
- the same Server contract force-escalates a graceful stop with a pending Recv,
  preserves the first graceful close reason, retires provisional/active
  Adapters exactly once, and publishes listener-before-Adapter-before-Hub-before-
  Server convergence with zero operation/notice/socket/pending-byte/owned-byte
  residue. Typed bind/listen failure and foreign-thread mutation must leave the
  Server capable of a complete owner-side stop
- the IOE-X12 contract uses one real loopback accept owner and at least two real
  worker EventLoops. It records worker identity from callbacks and proves four
  RoundRobin connections alternate owners, established echo/close callbacks
  never return to the accept owner, and physical close retires each exact load
- separate deterministic cases prove rotating-tie LeastConnections after one
  worker stays loaded, QueueLag avoids a worker with older queued work, and
  ConsistentHash maps repeated connections from the same peer IP to one stable
  worker. Tests must exercise the production selector for all policies except
  the explicitly mirrored LeastConnections load accounting
- with `maxPendingHandoffs == 1`, a blocked worker keeps one accepted envelope
  pending while the next accepted fd is rejected and closed without an
  overflow queue. A separately quiesced worker makes EventLoop post return a
  shutdown result; both cases must reconcile pending/load metrics and allow
  later listener-first stop
- X12 graceful/force and owner-quit cases require listener retirement before
  worker stop, exact settlement of all accepted posts, owner-side destruction
  of every worker Hub/Pump before thread-pool join, and zero listener, handoff,
  route, operation, notice, fd, pending-byte, and Engine-owned-byte residue
  under normal, focused repeat, ASan/UBSan, and TSan gates
- the IOE-X13 contract must use real AF_INET loopback sockets and prove the
  Engine emits `IoUringOperationKind::Connect` as one typed terminal operation,
  with the copied destination address and attempt lease retained through
  success, failure, or cancellation. No readiness Channel, blocking connect,
  detached worker, or borrowed post-submission address is allowed
- one success/echo case drives production `TcpClient` and the source-private
  Client against equivalent native peers and compares the common connected,
  message, disconnected, and terminal ordering. Backend-specific operation
  metrics remain explicit rather than fabricated for production epoll
- a refused endpoint must emit ConnectAttempt/ConnectFailed/RetryScheduled,
  recover after a real listener starts, reset bounded exponential backoff after
  success, and establish only one Adapter. A terminal no-retry failure must emit
  TerminalFailure once and allow callback-reentrant fresh connect
- an explicitly present zero timeout must deterministically cancel its exact
  submitted Connect before connection publication, emit ConnectTimeout once,
  and leave the socket owned until the cancellation target terminal. A restart
  or disconnect racing success must make the old generation stale, close or
  retire it once, and prevent old settlement, retry, or callback publication
- ConnectSuccess, ConnectFailed, timeout, connection, message, and close
  observers must re-enter restart/disconnect/stop without recursive drain or
  state overwrite. EventLoop quit with a pending attempt or established route
  must retire timer, Connect, Adapter, Route, Hub, operation, notice, socket,
  command, pending-byte, and Engine-owned-byte state before Shutdown
- the X13 focused contract runs repeatedly normally and with ASan/UBSan and
  TSan. Default Linux/Windows suites, Linux-only option rejection/install
  isolation, stable API zero-diff, scope, intent, CI inventory, and governance
  guards remain mandatory
- the IOE-X14 contract is one portable source registered in every default
  Linux and Windows test inventory. It must exercise the production
  `TcpServer`/`TcpClient` path selected by that platform and, only when Linux
  experimental support is enabled, execute equivalent source-private io_uring
  Server/Client runners in the same binary
- each X14 backend runner uses real AF_INET loopback and a single-operator raw
  peer socket to create deterministic finite output pressure. Any fixture that
  hands the socket between an owner callback and peer thread must prove that
  transfer with explicit release/acquire synchronization. The runner proves one
  typed overload rejection, accepted foreign Send before accepted foreign
  graceful Shutdown, high-water read pause, no inbound callback while paused,
  low-water resume, complete accepted-output delivery before peer-observed EOF,
  continued inbound delivery, one immutable `GracefulShutdown` close reason,
  and rejection of post-shutdown Send
- the portable comparison is semantic, not structural: callback owner/order,
  admission class, bytes, pause/resume, half-close, close reason, and terminal
  publication must agree. Readiness masks, IOCP packets, CQEs, operation counts,
  batching, native buffer sizes, and backend diagnostics must remain separately
  asserted or explicitly unavailable, never filled with invented equivalents
- X14 Server stop and Client stop/destruction must follow terminal connection
  observation. Production stop futures must be ready with zero active
  connections and pending output; experimental summaries must additionally
  reconcile zero listener, Connect, Adapter, Route, Hub, operation, notice,
  socket, timer, command, pending-byte, and Engine-owned-byte residue. The
  focused contract runs repeatedly normally and under the applicable Linux
  ASan/UBSan and TSan gates, while the full default suite runs on Windows IOCP
- X15 must prove two different install trees: default Linux contains no
  experimental target/header/library and rejects the requested component;
  experimental-enabled Linux contains the canonical
  `GameNet::experimental_io_uring` target plus the exact Server/Client header
  closure, and its independent consumer configures, builds, links, and runs
- Windows keeps rejecting `GAMENET_ENABLE_EXPERIMENTAL=ON` and therefore cannot
  publish an experimental component. The stable manifest and its v0.3
  compatibility diff remain unchanged; a separate experimental manifest must
  exactly fingerprint every installed experimental header and reject missing,
  extra, renamed, or modified entries
- a deterministic subprocess destruction contract holds a real accepted Recv
  and lease, invokes Pump destruction before physical stop, and requires an
  immediate fail-fast result rather than the historical 250 ms owner wait or
  source/slot cleanup. The normal owner-quit companion must still deliver one
  cancellation terminal, retain the lease through its consumer, publish the
  stop future, detach sources, and reach zero residue before legal destruction
- a drained standalone Engine destroyed from a real foreign thread must fail
  fast, proving owner-only destruction is independent of residue checks. A
  separate Pump construction contract closes the EventLoop readiness plane so
  ring-fd Channel registration rejects, then requires the original exception,
  zero attached lifecycle nodes, and clean empty-Engine rollback without
  termination or a timed wait
- injected completion-consumer/drive failure may mark `DrainedAfterFailure`
  only after the same terminal retirement and zero-residue checks. It cannot
  make an early destructor legal or detach Channel/lifecycle sources while an
  operation, notice, callback frame, or lease remains

## 5.1 Runtime Profile Required Test Examples
- the cross-Profile integration contract must execute the same real framed TCP
  echo and graceful-stop lifecycle through Profiles A/B/C/D, compare a narrow
  normalized outcome, and still assert each Profile's distinct handoff,
  cadence, cell-retirement, and owner/generation invariants; source-level API
  similarity or four individually green contracts is not evidence for an
  installed common Profile surface
- Profile A uses one real TcpServer with zero worker loops and proves accept,
  connection, framing, handler, reply, and close callbacks stay on the caller's
  EventLoop owner thread
- a five-frame input with a two-frame dispatch budget must advance as 2/2/1
  through same-owner continuations, preserve order, and report zero cross-domain
  handoffs
- saturation of the owner continuation queue closes with overload and never
  recursively invokes the remaining handler
- a deterministic handler wall-time violation is observed once, prevents the
  next frame from running, and converges to close; the test does not describe
  the violating sleep as safe application behavior
- protocol/response-size and TcpConnection output-hard-limit failures close
  with exact metrics and release all per-connection Profile state
- callback-reentrant stop revokes later handler/reply/continuation work and its
  TcpServer completion future becomes ready after EventLoop final drain
- the Profile support/example targets remain non-installed and the same-line
  stable API manifest remains unchanged
- Profile B uses one real TcpServer with at least two network workers plus a
  distinct logic EventLoop and observes two connection-owner executor ids,
  exactly one logic handler owner, and zero owner violations
- while the logic owner is deliberately occupied, ten accepted commands must
  create one producer wake post and nine merged wake requests; a two-command
  drain budget processes the backlog through bounded continuations and preserves
  per-connection reply order
- queue saturation during a slow logic handler closes the affected connection,
  stale queued/output generations are dropped, the queue returns to zero, and
  a fresh connection subsequently completes without inline fallback
- QueueFull, PayloadTooLarge, Stopped, logic post rejection, endpoint post
  rejection, and endpoint overload have distinct metrics/terminal policies;
  Accepted queue work is executed, generation-dropped, or counted at stop
- Profile B metrics expose network-to-logic and logic-to-network P99/P999,
  maximum observed queue age, queue high-water marks, coalescing, continuations,
  stale drops, and cross-domain handoff counts with fixed/bounded storage
- callback-active stop leaves the logic future pending until the handler
  returns, revokes its output, discards bounded backlog, and converges together
  with TcpServer's network future before either caller-owned loop is destroyed
- Profile C uses two real network owners plus one distinct tick owner and proves
  commands admitted before the first fixed-rate deadline invoke no handler
- five queued commands with `maxCommandsPerTick == 2` advance in 2/2/1 tick
  batches, preserve per-route output order, and never create a per-command logic
  wake or inline drain
- a deterministic tick overrun under `FixedRateBoundedCatchUp` executes no more
  than the configured consecutive catch-up count, records the replay, then
  records skipped cadence points and returns to a future deadline
- skip-missed mode records zero catch-up ticks; invalid skip/catch-up option
  combinations are rejected before TcpServer start
- Profile C queue saturation closes one route, queued generations become stale,
  later ticks return queue depth to zero, and a fresh route succeeds
- callback-active stop cancels/counts queued backlog, keeps the logic future
  pending through the committed tick and cadence retirement, revokes output,
  publishes bounded shutdown convergence time, and records exactly one timer-
  cancellation outcome even when the retiring callback races the accepted stop
  post
- Profile C metrics expose fixed-storage tick jitter P50/P99/P999, duration and
  queue-age P99/P999, max queue age/commands per tick, catch-up/skip/overrun,
  typed failures, owner violations, and exact cross-domain handoffs
- Profile D uses two real network owners and two distinct logic cell owners;
  metrics and handler observations prove the policies are independent
- one connection submits stable player/room/scene keys that select two cells
  without changing its captured network owner; two connections on different
  network owners using one key select the same logic cell
- each cell assigns a strict monotonic sequence. An event command behind a
  fixed-tick command cannot overtake it, while an event-only command on another
  cell progresses before that tick; cross-cell global ordering is not asserted
- per-cell saturation closes only the affected route and leaves a different
  cell able to accept/process work; key, router, queue, post, handler, output,
  and stopped failures remain distinguishable
- callback-active stop closes/discards every bounded cell queue, revokes active
  output, keeps the aggregate logic future pending through committed work and
  all timer retirements, then reports exact per-cell/aggregate drops
- Profile D support/example/benchmark targets remain non-installed and the
  structured stable public API diff remains empty

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
  interleaved wakeup, exact outstanding-operation release, distinct typed
  same-Channel read/write consumers, zero fake readiness callbacks, and
  terminal error preservation after the first consumer removes the Channel
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
  independent slot generations and socket ownership, distinct direct
  completion consumption with exact identities,
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
- same-runner performance regression must validate one unrecorded warmup per
  revision and scenario, record three adjacent baseline/candidate pairs with
  alternating first-run order, and fail closed when pair role, peer commit,
  warmup count, order rule, raw sample, or sample hash metadata drifts
- performance runners decode stdout as strict UTF-8 evidence but preserve
  localized/non-UTF-8 stderr with byte escapes; a diagnostic decoder failure
  cannot replace the benchmark process's real nonzero result
- the HP0 hot-path cost ledger must run one unrecorded warmup followed by at
  least ten recorded samples for every preregistered scenario, preserve each
  raw child JSON document and SHA-256, and bind the ledger to one exact clean
  commit plus every executable hash
- `gamenet.hot_path_cost.v1` records a fixed key set for cycles, instructions,
  LLC load misses, allocations, copied bytes, syscalls, wakeups, generic posts,
  cross-domain handoffs, P50/P99/P999, maximum queue age, RSS, overload
  recovery, and shutdown convergence; an unavailable counter is `null` with a
  non-empty reason and must never be represented by a synthetic zero
- fixed-lab hot-path evidence is native Linux/epoll or native Windows/IOCP,
  uses a named self-hosted runner, fixed CPU affinity/frequency policy,
  Release build and compiler identity, and rejects WSL, dirty worktrees,
  missing preregistered scenarios, missing required counters, shortened loads,
  fewer than ten samples, or parameter drift; development evidence may retain
  those limitations but is never promotion eligible
- while HP0 fixed-lab evidence is incomplete, at most one HP1-HP8 runtime
  prototype may be labeled `EXPERIMENTAL-PRESTUDY`; it must be default-off,
  non-installed, non-exported, absent from the default production Core data
  path, public manifests, and release assets, and it cannot replace the current
  callback-and-mutex baseline
- an `EXPERIMENTAL-PRESTUDY` runtime prototype must name a deterministic test
  file before implementation and cover its owner, ownership/release,
  callback re-entry, cross-thread marshal, bounded admission, failure paths,
  shutdown settlement, and zero-residue obligations
- prestudy design and benchmark scaffolding may be prepared out of milestone
  order, but runtime tests must use real satisfied dependencies; a mock or
  synthetic downstream component cannot validate a composed HP7/HP8 claim
- prestudy measurements are development evidence only and cannot satisfy an
  HP milestone, dependency gate, fixed-lab baseline, 5%/3% promotion decision,
  default-path switch, installed API addition, version, or release decision
- after HP0 closes, a retained prestudy candidate must enter the formal HP
  slice and rerun its contracts and performance comparison against the fixed
  exact-commit baseline; no prestudy result is grandfathered into verification
- the active HP1 `EXPERIMENTAL-PRESTUDY` contract is
  `tests/contract/protocol/test_packet_framer_view.cpp`; it must use a real
  `Buffer` readable region and prove borrowed-view expiry discipline, explicit
  one-copy retain, zero owner-local allocations without retain, and zero post,
  wakeup, or handoff behavior by construction
- HP1 differential cases cover partial prefix/payload, sticky and empty frames,
  frame-count and frame-byte budgets, a valid frame followed by an oversized
  prefix with zero visitor side effects, sticky fault/reset, visitor exception,
  nested visit/reset rejection, exact `consumedBytes`, and randomized valid
  streams against legacy `PacketFramer::push()`
- the HP1 benchmark is default-off, non-installed, non-exported, preserves the
  unchanged legacy callback-copy path, and reports raw checksums, payload/
  copied/retained byte accounting, iteration count, and elapsed time for both
  paths; its development output cannot satisfy the HP0 5%/3% promotion gate
- the active HP2 `EXPERIMENTAL-PRESTUDY` contracts are
  `tests/contract/runtime_model/test_spsc_mailbox.cpp` and
  `tests/contract/event_loop/test_event_loop_mailbox_source.cpp`; they use real
  producer/consumer threads and move-only HP1 OwnedPacket values rather than a
  model-only vector of copyable integers
- HP2 mailbox tests cover overflow-safe topology memory rejection, power-of-two
  capacity, in-place construction, QueueFull, FIFO wraparound, batch budgets,
  concurrent release/acquire publication, callback re-entry, visitor exception,
  stop versus producer, explicit cancellation, and exact
  `accepted == processed + cancelled` zero-residue settlement
- HP2 source tests deterministically place a producer before and after the
  consumer clears notification-pending, prove at most one physical notification
  per non-empty burst without lost wakeup, reject recursive drain, invalidate
  stale generations on detach/replacement, and distinguish QueueFull, Stopped,
  and OwnerUnavailable
- the HP2 benchmark preserves callback+mutex as baseline, records both mailbox
  directions, allocations, copied bytes, notifications, generic posts,
  handoffs, throughput, queue age and shutdown residue, and remains
  development-only until HP0 fixed-lab pairing
- the active HP3 `EXPERIMENTAL-PRESTUDY` contract is
  `tests/contract/io_engine/test_epoll_slot_dispatch.cpp`; it covers fixed
  capacity, packed index/generation identity, same-generation interest update,
  remove/reuse generation, stale/malformed/wakeup filtering, O(1) duplicate
  merge, batch limit, epoch wrap, exact cancellation and zero residue
- HP3 decode invokes no callback and performs no fd hash lookup. A deterministic
  active-batch case cancels and reuses a later target before dispatch, then
  proves `isCurrent` rejects the old notice and cannot reach the replacement
- HP3 foreign-thread mutation is rejected. Stop seals new registration and
  requires every borrowed target registration to cancel before terminal
  settlement; the prototype owns no real epoll fd or cross-thread wakeup
- the HP3 benchmark preserves the current fd-map plus linear-merge algorithm as
  baseline and reports native events, delivered unique notices, map lookups,
  merge probes, slot probes, stale/filtered counts, elapsed time and checksum;
  portable timing is development-only and cannot replace native Linux evidence
- the active HP4 `EXPERIMENTAL-PRESTUDY` contract is
  `tests/contract/tcp_connection/test_output_segment_chain.cpp`; it covers
  fixed capacity, owned/shared storage, non-concatenated views, 16-segment and
  64-KiB batches, direct-empty prepare, FIFO and partial completion across
  segment boundaries without suffix copy
- HP4 contract paths reject nested/in-flight prepare, invalid completion,
  foreign-owner mutation, capacity/stopped admission and cancellation during
  an in-flight batch; stop plus explicit discard must reconcile accepted bytes
  to completed plus discarded with zero residue
- an HP4 broadcast case shares one immutable payload across many owner-local
  chains, validates route generation before enqueue, reports stale/rejected
  targets, and proves no per-endpoint payload copy or shared-storage mutation
- the HP4 benchmark preserves per-endpoint header+payload concatenation as the
  baseline and reports copied bytes, allocation events, batch/views, elapsed
  time, checksum and zero residue. It performs no real socket syscall and
  cannot substitute for native segmented-write or broadcast integration tests
- the active HP5 `EXPERIMENTAL-PRESTUDY` contract is
  `tests/contract/tcp_connection/test_credit_lease.cpp`; it proves owner-only
  mutation, slot-generation reuse, typed connection/loop/server/global
  rejection, later-scope rollback, concurrent parent no-overshoot, idle-credit
  reuse/trim, cancel/retire/stop and exact zero-residue settlement
- the HP5 benchmark compares the current modeled four-scope per-message atomic
  chain with owner-local connection/loop accounting and batched server/global
  lease operations. It reports modeled atomic mutations, elapsed time,
  checksum, accepted/released/discarded bytes and residue; it cannot replace
  native TcpConnection, broadcast, capacity or fixed-lab evidence
- the active HP6-A `EXPERIMENTAL-PRESTUDY` contract is
  `tests/contract/event_loop/test_adaptive_phase_scheduler.cpp`; it covers
  option/owner/stopped rejection, count/time/deficit bounds, age boost,
  weighted saturated progress, minimum control/lifecycle service, poll-zero
  continuation, stop and zero residue
- the HP6 benchmark runs fixed-count and adaptive quota planners over identical
  finite synthetic work and reports completion/checksum equality, simulated
  total/max-round cost, first control/lifecycle latency, planner overhead,
  rounds and zero-poll continuations. Estimated cost cannot substitute for real
  EventLoop queue-age/P99/P999 evidence
- HP6-B pools and HP6-C hot/cold layout remain `SKIPPED-BY-EVIDENCE` until HP0
  identifies a qualifying allocation, cache-miss or bytes-per-connection hotspot
- HP8 build-profile governance is verified by
  `tests/cmake/test_build_governance_contract.py`: portable/native tuning must
  be explicit and mutually observable, PGO generate/use share one training
  build tree, sanitizer excludes PGO, benchmark instrumentation retains frame
  pointers, and all named presets parse through CMake
- HP8 backend promotion remains `DEFER` until clean exact-commit HP0 ledgers and
  a post-user-data-path epoll/IOCP/io_uring comparison exist. Build presets or
  deployment tuning alone are never backend promotion evidence
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
  each healthy-probe batch must keep successfully connected sockets open until
  the cumulative server-accept count converges, then run exact echo/abortive
  close and wait for cumulative server-close convergence, so a client I/O
  deadline cannot make exact lifecycle accounting unreachable;
  retained-memory sampling must group connections by owner and enqueue exactly
  one owner-affine snapshot batch per loop so evidence collection does not
  starve the concurrent healthy probes;
  and 10k candidate versus 100k dedicated endpoint-attempt parameter drift
  must be rejected by structured evidence guards
- a production promotion artifact must revalidate retained raw capacity and
  endurance evidence rather than trusting a copied summary; candidate mode
  requires 10k plus 1h unless the project owner records an explicit candidate
  waiver, in which case the exact 10k pair is still required; release mode
  requires dedicated 100k plus same-SHA 1h/3h unless the owner records an
  explicit release waiver, in which case the exact dedicated 100k pair is still
  required; waiver artifacts must use status `waived`, and SHA, workflow run,
  rerun attempt, stage, profile, duration, hash, waiver approval, or source-
  inventory drift must fail closed
- a nonzero or invalid-JSON capacity-profile sample must retain its raw stdout
  under the run artifact; structured failures report the document error plus
  false checks, and stderr-only or toolchain-only evidence cannot support
  capacity remediation
- a capacity-profile success explicitly flushes and checks the complete stdout
  document before returning zero; a single stdio-buffer prefix is invalid
  evidence even when the child process otherwise reports success

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
  cannot substitute for the fixed 1-hour or 3-hour gate
- an owner-approved candidate or release waiver is missing-evidence metadata,
  not a shortened substitute or a successful endurance result; it authorizes
  only its named promotion stage
