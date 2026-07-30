---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: TcpConnection

## 1. Intent
TcpConnection models one TCP connection bound to one EventLoop.
It remains the lifecycle center of a connection: it owns the per-connection
socket/channel/buffer state, exposes the public connection API, and converges
read/write/error/close into one safe teardown model while preserving reactor semantics.

TcpConnection should stay small enough to reason about.
Transport details, coroutine waiter bookkeeping, and backpressure enforcement
may collaborate with dedicated loop-owned helper components instead of accumulating
inline inside TcpConnection.

---

## 2. Responsibilities
- own connection-local socket/channel/buffer state
- own the public connection state machine
- expose cross-thread-safe snapshot observation of the public connection state
- translate read/write/close/error events into unified connection state changes
- expose send/shutdown APIs that preserve owner-thread execution
- bound admitted output bytes across buffered and not-yet-executed cross-thread
  sends, returning an explicit overload result before additional payload memory
  is admitted
- when installed by TcpServer, reserve each accepted output byte through the
  fixed connection -> owner-loop -> server -> optional shared-global budget
  chain without a hot-path mutex; report the first rejecting scope explicitly
- on Windows, move admitted payloads into a loop-owned stable-segment queue
  instead of mirroring the complete output Buffer into transport-private write
  storage
- on Windows, allocate read backing storage only when the first `WSARecv` is
  posted, retain at most one fixed 4 KiB connection-local chunk, and release it
  after final completion drain
- bound unread input bytes and close through the normal lifecycle path when an
  application callback leaves the input buffer at its configured hard limit
- expose force-close style teardown entry that still converges on the same close path
- expose connection context as loop-owned mutable state, not cross-thread storage
- dispatch user-visible connection/message/write-complete/high-water/close callbacks
- count optional high-water/write-complete notifications that cannot be
  admitted to the owner-loop normal queue without allowing that diagnostic
  loss to interrupt connection progress
- coordinate loop-owned helper components for transport, coroutine waiting,
  and backpressure without bypassing EventLoop scheduling
- attach one dynamic EventLoop lifecycle node after initial Channel
  registration and detach it only after socket close, completion drain,
  Channel removal, and final callback publication converge
- publish a structured close result that preserves the semantic reason and an
  optional native error without making upper layers parse log text

---

## 3. Non-Responsibilities
- does not own EventLoop
- does not own TcpServer
- does not invent a separate coroutine scheduler
- does not perform application protocol parsing
- should not embed every optional transport variant or waiting policy as ad-hoc
  inline state when a dedicated loop-owned collaborator can hold that responsibility

---

## 4. Core Invariants
- one TcpConnection belongs to exactly one EventLoop
- connection state mutation happens on owner loop thread
- `connected()` and `disconnected()` only observe the atomic public state;
  they do not grant cross-thread access to any other connection-owned state
- close and error teardown converge on one safe path
- channel registration is removed before effective destruction
- helper component state that mutates connection behavior is still owned by the same loop
- backpressure-driven read suspend/resume only changes Channel interest on owner loop thread
- every accepted output byte is reserved exactly once and released exactly once
  after write completion or close-time discard; the configured hard limit is
  never exceeded even by concurrent cross-thread send admission
- a hierarchical reservation never exceeds any configured hard limit. Failure
  at a later scope rolls back every earlier scope before `trySend()` returns,
  and an accepted byte releases every scope exactly once
- each shared output budget enters overload only after capacity rejection above
  its recovery threshold and resumes admission only after pending bytes return
  to or below that threshold; one request larger than an otherwise idle budget
  is rejected without latching unrelated small sends
- current/peak/rejection/overload snapshots are atomic observations only. They
  grant no right to mutate owner-loop connection state
- the Windows pending-write queue contains stable owned string segments. One
  segment is appended once, remains at a stable address while `WSASend`
  observes it, advances only its front offset on partial completion, and is
  removed only after every byte in that segment completes
- each Windows `WSASend` submission references at most
  `std::numeric_limits<ULONG>::max()` bytes from the current front segment;
  completing that chunk never copies or compacts the uncompleted suffix
- Windows buffered-output accounting is the exact sum of remaining segment
  bytes. It drives high/low-water transitions while the atomic
  `pendingOutputBytes` additionally includes accepted cross-thread payloads
  that have not yet reached the owner loop
- output-buffer mutation, backpressure enforcement, and write-interest /
  platform write initiation complete before an optional high-water
  notification is submitted
- output drain disables write interest and performs a pending disconnecting
  half-close before an optional write-complete notification is submitted
- rejected optional notification submission increments one connection-local
  atomic dropped-notification counter and never rolls back mandatory state
- each socket read is capped by the input buffer's remaining allowance; the
  input Buffer never grows past the configured connection limit
- each Windows `WSARecv` references at most 4 KiB of connection-local backing
  storage. That allocation is absent before the first post, remains stable
  while a completion is pending, is reused without growth across normal read
  completions, and is released only after no read completion obligation remains
- Windows read backing is never returned across EventLoops: this slice uses
  one bounded allocation owned by the connection's IOCP transport rather than
  a shared pool
- coroutine resume returns through EventLoop scheduling
- transport-specific behavior (plain TCP or TLS) must not change callback ordering
  or close-path convergence
- on Windows, successful or pending `WSARecv` / `WSASend` submission establishes
  exactly one completion obligation; a synchronous non-pending failure
  establishes none and is returned directly to TcpConnection
- close is an explicit owner-loop state machine:
  `Open -> Closing -> SocketClosed -> CompletionDraining -> Closed`; public
  connected/disconnecting snapshots remain compatible views of that model
- exactly one semantic close reason wins at the first transition to Closing;
  later errors/cancel completions may add diagnostic native-error context but
  cannot replace the winning reason or duplicate callbacks
- the socket is explicitly closed on the owner loop after read/write
  cancellation has been requested; destruction is not the mechanism that
  initiates close
- on Windows, closing the socket does not release transport operation storage;
  `CompletionDraining` remains attached to the EventLoop lifecycle hub until
  every kernel-owned completion obligation has been consumed
- on Linux, Channel interest disable and Poller removal complete on the owner
  loop before explicit socket close releases the numeric fd; both operations
  normally converge in the same lifecycle visit, while preserving identical
  callback/result order
- disconnected and close callbacks run only after the socket is closed and
  completion obligations are zero; TcpServer/TcpClient removal observes the
  structured close result

---

## 5. Collaboration
- TcpConnection owns Socket, Channel, input/output Buffer, and callback slots
- TcpConnection may delegate read/write/shutdown details to ConnectionTransport
- the existing Core-private Windows IOCP transport owns the stable segment
  container on behalf of TcpConnection; TcpConnection remains responsible for
  reservation, callback, backpressure, and lifecycle semantics
- the same IOCP transport uniquely owns its optional read chunk; TcpConnection
  supplies remaining input capacity and requests final release only after
  completion drain
- TcpConnection may delegate coroutine waiter state to ConnectionAwaiterRegistry
- TcpConnection may delegate threshold-based read pause/resume to
  ConnectionBackpressureController
- TcpConnection remains the only component that changes the public connection state
  or invokes the final close callback used by TcpServer/TcpClient
- TcpConnection owns its local reservation count and borrows shared ownership
  of immutable loop/server/global budget identities. TcpServer owns the loop
  and server budget configuration; an optional global budget may be shared by
  multiple servers and outlive any one server until all accepted bytes release
- EventLoop owns lifecycle-node scheduling/storage; TcpConnection owns the
  node registration generation and requests detach during final owner-loop
  cleanup

---

## 6. Threading Rules
- handleRead/handleWrite/handleClose/handleError run on owner loop thread
- cross-thread send/shutdown must marshal back into the loop
- cross-thread send copies the admitted bytes into one immutable payload before
  returning Accepted; the executor moves that same allocation into the owner-
  loop Windows segment queue without another complete-payload copy
- hierarchical output admission is cross-thread-safe atomic value accounting;
  it does not mutate Channel, Buffer, segment, server-map, or EventLoop state
  and does not invoke callbacks
- `connected()` and `disconnected()` may be called from any thread and return
  a point-in-time state snapshot
- `droppedNotificationCount()` may be called from any thread and returns a
  point-in-time cumulative snapshot
- socket-option mutation, connection context access, callback replacement,
  `connectEstablished()`, and `connectDestroyed()` are owner-loop-only
- callback setters are setup-time configuration before establishment; the
  close callback may only be replaced later on the owner loop during teardown
- setContext/getContext are owner-loop-only; cross-thread users must marshal
  context access through `runInLoop()` or `queueInLoop()`
- helper components must not create a second mutable thread domain
- backpressure threshold evaluation and read-interest toggling happen on the owner loop
- high-water and write-complete callbacks remain deferred owner-loop
  callbacks when admitted; normal-queue saturation may drop them explicitly
- Windows read resume is mandatory owner-loop work and runs inline from the
  low-water transition instead of depending on ordinary queue admission; a
  buffered message callback on that path may therefore re-enter connection APIs
- Windows read-chunk allocation, submission, reuse, and final release are
  owner-loop-only and never transfer storage to another loop
- after such Windows callback re-entry, the outer write handler rechecks
  disconnected/force-close state and pending-overlapped-write state before it
  performs any further transition
- await readiness checks must not inspect loop-owned mutable state off-thread
- transport handshake/read/write/shutdown logic runs on the owner loop thread only
- cross-thread force-close/error/owner-shutdown paths update a synchronized
  close request and call lifecycle `signal()`; they never depend on ordinary
  functor admission for terminal progress
- lifecycle-node callbacks run on the connection owner loop and may consume
  coalesced send-shutdown, force-close, socket-close, and completion-drain
  work; every visit must re-read the explicit close state

---

## 7. Failure Semantics
- fatal read/write errors should produce explicit teardown
- peer-close write failure must reach the normal error/close path without a
  process-wide `SIGPIPE` termination or a global signal-policy side effect
- repeated close/error handling should be guarded or idempotent
- error-triggered teardown remains single-shot even if user code re-enters a
  teardown API from the close callback
- pending IOCP read/write operations are canceled and drained on the owner loop
  before force-close teardown releases connection-owned operation storage
- EventLoop quit cannot strand the connection between cancellation and
  completion consumption: the attached lifecycle node keeps the loop in
  Quiescing until the operation count reaches zero
- a synchronous Windows read/write submission failure invokes the same
  `handleError()` -> `handleClose()` convergence immediately on the owner loop;
  it does not set Channel `revents` and wait for a nonexistent completion
- failure to allocate the bounded Windows read chunk is reported as
  `WSAENOBUFS` before submission and therefore creates no kernel completion
  obligation
- normal, error, and `ERROR_OPERATION_ABORTED` completion consumption clears
  the corresponding pending obligation once; repeated close/error entry remains
  single-shot
- Windows synchronous write-submit failure retains the queued segment only
  until normal close cleanup; a pending/canceled write retains its referenced
  front segment until the real completion is consumed, after which close may
  discard the exact remaining segment bytes and release their reservations
- timeout-driven close should reuse the normal close path rather than inventing a side channel
- backpressure throttling should resume automatically after the output buffer drains below the low-water threshold
- disconnected state should block unsafe user-visible actions
- shutdown waits for pending output to drain before issuing the socket
  half-close
- repeated shutdown requests are idempotent and must not duplicate the
  owner-loop half-close, write-complete, disconnected, or close callbacks
- high-water callback fires once when output crosses the threshold and is
  delivered on the owner loop when its optional queue submission is admitted
- high-water/write-complete queue rejection, callback capture failure, or
  allocation failure is contained at the optional notification boundary,
  increments `droppedNotificationCount()`, and does not throw into the active
  read/write/state-transition path
- a dropped high-water notification does not prevent read backpressure or
  EPOLLOUT/WSASend progress; a dropped write-complete notification does not
  prevent a disconnecting connection from half-closing
- `trySend()` reports `Overloaded` for the connection hard limit and distinct
  loop/server/global overload results for the first rejecting shared scope,
  before queue/buffer growth; the legacy `send()` API uses the same bounded
  admission path
- reaching the configured input hard limit after message dispatch is treated
  as connection overload and converges on the existing close path
- helper-component failure must still converge on TcpConnection's existing error/close model
- kConnected publication occurs only after initial Channel registration
  succeeds; a registration exception leaves the object in kConnecting so
  rollback cannot emit a disconnected callback for a connection that was
  never established
- connection-established, message, high-water, and write-complete callback
  exceptions are reported and force-close only the offending connection
- disconnected and close callback exceptions are reported and contained;
  remaining lifecycle callbacks and remove-before-destroy cleanup still run
- an exception observer is diagnostic only: if it throws, the fixed connection
  close/cleanup policy still applies
- structured close reasons distinguish at least PeerEof, Reset,
  ConnectTimeout, InputLimit, OutputOverload, AdmissionPolicy,
  GracefulShutdown, ForcedShutdown, CallbackFailure, and InternalError
- the first close transition publishes one immutable `TcpConnectionCloseInfo`;
  callbacks receive the same semantic reason even when later cancellation
  completions carry platform-specific abort errors

---

## 8. Extension Points
- ConnectionTransport for plain TCP vs TLS transport behavior
- ConnectionAwaiterRegistry for coroutine read/write/close awaiters
- ConnectionBackpressureController for threshold-based read throttling
- optional high-water-mark notification callback
- future backpressure metrics and observability hooks

---

## 9. Test Contracts
- cross-thread send executes on owner loop thread
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_state.cpp`
  verifies non-owner-thread state observation across connect and close transitions
- the TcpConnection thread-contract guard verifies state storage is atomic and
  owner-loop-only mutators assert their thread affinity
- read/write error path converges on safe close handling
- peer close or reset converges on the normal close path with one
  disconnected callback and one close callback
- repeated forceClose calls are idempotent and do not duplicate connection or
  close callbacks
- cross-thread force-close marshals back to the owner loop and converges on safe close handling
- connection context access is documented and asserted as owner-loop-only
- `tests/contract/tcp_connection/test_tcp_connection_peer_close.cpp` verifies
  peer EOF/close teardown does not duplicate close-path callbacks
- `tests/contract/tcp_connection/test_tcp_connection_peer_reset.cpp` verifies
  peer reset/error teardown remains single-shot when teardown is re-entered
  from the close callback
- write-complete callback is queued after send returns and does not re-enter
  the caller's send stack
- `tests/contract/tcp_connection/test_tcp_connection_write_complete_ordering.cpp`
  verifies write-complete callback ordering on the owner loop
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_send.cpp`
  verifies cross-thread send copies payload ownership, marshals to the owner
  loop, and delivers write-complete on that loop
- `tests/contract/tcp_connection/test_tcp_connection_send_after_close.cpp`
  verifies owner and non-owner send() calls after disconnection are ignored
  without writing to the peer or firing write-complete callbacks; it also
  verifies an unconsumed input stream is read only to the configured cap and
  then closed through the normal lifecycle path
- `tests/contract/tcp_connection/test_tcp_connection_shutdown_pending_output.cpp`
  verifies shutdown waits for pending output to drain before peer EOF
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp`
  verifies cross-thread shutdown marshals to the owner loop, drains pending
  output, and then half-closes exactly once
- `tests/contract/tcp_connection/test_tcp_connection_repeated_shutdown.cpp`
  verifies repeated owner and non-owner shutdown requests drain pending output
  and converge on one owner-loop half-close
- `tests/contract/tcp_connection/test_tcp_connection_high_water_mark.cpp`
  verifies high-water callback threshold delivery, hard-limit rejection,
  high/low-water read pause/resume on the owner loop, and a resumed message
  callback that re-enters `trySend()` plus `forceClose()`
- `tests/contract/tcp_connection/test_tcp_connection_queue_saturation.cpp`
  fills both normal and reserved pending-functor capacity and proves that
  rejected high-water/write-complete notifications increment the dedicated
  dropped counter without throwing; mandatory backpressure, write progress,
  disconnecting half-close, and output-reservation release still converge
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_send.cpp`
  verifies concurrent-domain admission reserves bytes before owner-loop
  execution and rejects a second send that would exceed the same hard limit
- `tests/contract/tcp_connection/test_tcp_output_memory_budget.cpp` verifies
  mutex-free hard limits, hysteretic recovery, concurrent no-overshoot,
  hierarchical rollback, and exact release to zero
- `tests/contract/tcp_server/test_tcp_server_output_memory.cpp` verifies
  TcpServer injects distinct loop/server/optional shared-global budgets,
  reports the rejecting scope, exposes low-frequency snapshots, and releases
  all scopes after close/stop
- `tests/contract/tcp_connection/test_tcp_connection_repeated_force_close.cpp`
  verifies repeated forceClose teardown remains single-shot
- `tests/contract/tcp_connection/test_tcp_connection_repeated_connect_destroyed.cpp`
  verifies forceClose removes the Linux Channel before the close callback can
  reuse the released numeric fd for a replacement registration, and repeated
  connectDestroyed teardown does not remove that replacement or leave stale
  registration behind
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_force_close_soak.cpp`
  repeats cross-thread forceClose and verifies teardown marshals to the owner
  loop without duplicating callbacks
- `tests/contract/tcp_connection/test_tcp_connection_force_close_pending_read.cpp`
  verifies forceClose cancels a pending read and waits for the owner-loop
  completion path before connection destruction
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_force_close_pending_read.cpp`
  repeats non-owner forceClose during pending read and verifies the request
  marshals to the owner loop before cancel/drain releases connection storage
- `tests/contract/tcp_connection/test_tcp_connection_force_close_pending_read_mixed_timing_soak.cpp`
  alternates immediate and delayed owner/non-owner forceClose timing during a
  pending read and verifies cancel/close convergence remains single-shot
- `tests/contract/tcp_connection/test_tcp_connection_force_close_pending_write_soak.cpp`
  repeats forceClose during a large pending write and verifies IO cancellation
  drains before connection-owned operation storage is released
- `tests/contract/tcp_connection/test_tcp_connection_force_close_pending_write_mixed_timing_soak.cpp`
  alternates immediate and delayed owner/non-owner forceClose timing during a
  large pending write and verifies cancel/close convergence remains single-shot
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_force_close_pending_write.cpp`
  verifies non-owner-thread forceClose marshals to the owner loop while a large
  write is pending and still drains cancel/close before destruction
- `tests/contract/tcp_connection/test_tcp_connection_iocp_sync_error.cpp`
  injects synchronous `WSAENOBUFS` / `WSAECONNRESET` read and write submission
  failures, proves they close immediately without phantom readiness, and
  verifies cancellation of the other real pending operation reports
  `ERROR_OPERATION_ABORTED` before one final teardown
- `tests/contract/tcp_connection/test_tcp_connection_iocp_segmented_write.cpp`
  verifies multiple cross-thread payload allocations move into distinct stable
  owner-loop segments, preserve byte order, obey the configured test chunk,
  and release exact pending-byte reservations
- `tests/contract/tcp_connection/test_tcp_connection_iocp_partial_write.cpp`
  forces one large segment through many bounded IOCP submissions while a slow
  peer drains, proving partial completions advance the front offset without a
  suffix copy and converge to one write-complete callback
- `tests/contract/tcp_connection/test_tcp_connection_iocp_read_storage.cpp`
  proves read storage is absent before first post, capped and reused at 4 KiB,
  bounds each completion, preserves input-limit behavior, survives pending-read
  cancellation, and is released before final close publication
- `tests/contract/tcp_connection/test_tcp_connection_completion_drain.cpp`
  verifies explicit socket close precedes disconnected publication, pending
  operations retain storage, and one final lifecycle detach follows drain
- `tests/contract/tcp_connection/test_tcp_connection_close_reason.cpp`
  verifies first-reason-wins, native-error preservation, callback failure,
  overload, graceful, forced, peer EOF, and reset mapping
- `tests/integration/tcp/test_iocp_quit_completion_drain.cpp` races force close,
  socket cancellation, and EventLoop quit and proves the real completion is
  consumed before Shutdown
- high-water to low-water drain path pauses and resumes read processing on the owner loop
- coroutine awaiters resume through EventLoop rather than arbitrary caller thread
- repeated teardown does not leave stale registration behind
- swapping transport implementation does not change connection callback ordering
  or teardown semantics
- `tests/contract/tcp_server/test_tcp_server_contract.cpp` verifies
  a throwing message callback closes only its connection, reports its exact
  callback source on the connection owner loop, and leaves TcpServer able to
  accept and serve a later connection; it also verifies a throwing disconnect
  callback cannot suppress close bookkeeping

---

## 10. Review Checklist
- Is all mutable connection state still loop-owned?
- Do close and error share one teardown model?
- Can callbacks or coroutine resumes outlive safe ownership?
- Is registration removed before destruction?
- Are optional behaviors delegated cleanly without weakening the public lifecycle model?
