---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: Connector

## 1. Intent
Connector is the active-connect adapter for TcpClient,
symmetric to Acceptor's role for TcpServer.
It initiates non-blocking TCP connect, handles EINPROGRESS,
detects success or failure via writable-event polling,
and delivers the connected fd upward through a narrow callback boundary.

---

## 2. Responsibilities
- create a non-blocking socket and initiate connect
- handle EINPROGRESS by registering writable interest on a Channel
- detect connect completion or failure via getsockopt(SO_ERROR)
- deliver connected fd to upper layer (TcpClient) on the owner loop thread
- support configurable retry with backoff on connect failure
- clean up socket and Channel registration on failure, cancel, or destruction

---

## 3. Non-Responsibilities
- does not create TcpConnection
- does not own TcpClient or EventLoop
- does not implement application protocol logic
- does not perform DNS resolution
- does not manage post-connect I/O

---

## 4. Core Invariants
- Connector belongs to exactly one EventLoop
- Connector is an owner-loop-only state machine; it is not itself the
  cross-thread facade
- Channel mutation happens only on the owner loop thread
- at most one connect attempt is in flight at any time
- each start / stop / restart boundary advances an owner-loop request
  generation; retry, timeout, and connect-completion work from an older
  generation may clean up its own resources but may not publish or resurrect
  a connection
- diagnostic and new-connection callbacks may synchronously re-enter
  start / stop / restart; after such a callback returns, an older completion
  may conditionally settle its own `kConnected` state but must never overwrite
  `kConnecting` or any other state already published by a newer generation
- after a connecting Channel is removed, Connector releases member ownership
  immediately even when actual Channel destruction must be deferred until the
  current event callback returns
- connected fd is either delivered upward or closed explicitly (no leak)
- retry backoff parameters are explicitly configurable
- destruction removes Channel from Poller before releasing the socket

---

## 5. Collaboration
- owned by TcpClient
- uses Channel for writable-event notification during EINPROGRESS
- relies on EventLoop for Channel registration and timer-based retry delays
- delivers connected fd to TcpClient via a new-connection callback
- does not interact with TcpConnection directly

---

## 6. Threading Rules
- start() / stop() / restart(), retry configuration, and callback replacement
  are owner-loop-thread-only operations and assert that affinity before
  reading or mutating loop-owned state
- TcpClient uses its cached EventLoopExecutor to admit cross-thread work, then
  invokes Connector methods only from the admitted owner-loop callback
- state() is the sole cross-thread Connector state observer and returns an
  atomic snapshot; the snapshot is diagnostic and grants no mutation right
- retryEnabled() is an atomic diagnostic/configuration snapshot; mutation
  remains owner-loop-only
- handleWrite / handleError (Channel callbacks) run on the owner loop thread
- retry timer callback runs on the owner loop thread

---

## 7. Ownership Rules
- TcpClient owns Connector
- Connector owns the connecting socket fd until it is delivered upward
  or closed on failure
- connected-fd ownership transfers at new-connection callback entry; the
  receiver must install an RAII owner before fallible work, and Connector never
  closes the fd after that handoff (including callback-exception unwinding)
- Connector does not publish the removed Channel's numeric IOCP association
  into Poller bookkeeping before the receiver proves it will synchronously
  register a replacement Channel; TcpClient performs that narrow preservation
  only after its fallible socket inspection and connection construction
- Connector owns its Channel registration during the connect attempt
- Channel must be removed before Connector destruction

---

## 8. Failure Semantics
- socket creation, Windows unspecified bind/extension lookup, connect-context
  update, and local/peer-address lookup failures stay on the normal explicit
  connect-failed/retry path; they must not terminate the process
- connector event observers are diagnostic; observer exceptions are logged and
  contained without interrupting the connect/retry state transition
- connector event observers may synchronously mutate the owner-loop state
  machine; every continuation after an observer call must revalidate its
  captured generation and use a conditional state transition so stale success
  cleanup cannot clobber a re-entrant restart
- a throwing new-connection callback is logged and leaves Connector
  disconnected; the callback-side RAII owner closes or retains the fd exactly
  once because ownership already transferred at callback entry
- receiver-side handoff failure is allowed to propagate back through the
  callback boundary; Connector conditionally settles only that completed
  generation and cannot retain kConnected or overwrite a newer re-entrant
  attempt
- a callback that closes or rejects the transferred fd before replacement
  Channel registration leaves no preserved numeric SOCKET entry in IocpPoller
- connect refused / network unreachable / timeout:
  close the socket, invoke error notification, optionally schedule retry
- after the failure reason event, Connector emits exactly one
  `RetryScheduled` or `TerminalFailure` decision event for the current
  generation; TcpClient does not infer terminality by enqueueing a later
  ordinary task
- EINTR during connect: retry immediately
- getsockopt(SO_ERROR) non-zero after writable event: treat as connect failure
- self-connect detection (local addr == peer addr): close and retry
- destruction during pending connect: close socket, remove Channel, no callback
- destruction during pending retry timer: cancel timer, no callback
- stop invalidates the current request generation before canceling timers or
  an in-flight connect; a canceled retry callback cannot start a later attempt
- Windows cancellation completion remains responsible for releasing its own
  Channel / operation storage even when its generation is stale; generation
  rejection applies to publication and retry, not mandatory cleanup

---

## 9. State Machine
Connector operates as an explicit state machine:
- **kDisconnected**: idle, no socket, no Channel
- **kConnecting**: socket created, connect initiated, Channel registered
  for writable event (EINPROGRESS path)
- **kConnected**: fd delivered to upper layer, Connector is idle
  (socket ownership transferred)

Transitions:
- kDisconnected → kConnecting: start() called
- kConnecting → kConnected: writable event with SO_ERROR == 0
- kConnecting → kDisconnected: connect failure (with optional retry timer)
- kConnected → kDisconnected: stop() called (upper layer manages the connection)
- any state → kDisconnected: destruction

Request-generation rules:
- start, stop, and restart advance a monotonically increasing generation on
  the owner loop
- Channel callbacks, connect-timeout callbacks, and retry timers capture the
  generation that created them
- a callback with a stale generation cannot deliver an fd or schedule retry
- a stale success continuation may close only its still-owned fd and may change
  `kConnected` to `kDisconnected` only if that exact state is still current;
  it cannot store over a newer generation's `kConnecting`
- a newer start received while Windows cancellation is draining is remembered
  and begins only after the old completion has released the connecting slot
- restart resets exponential backoff to the configured `initRetryDelay`, not
  to a compiled-in default

---

## 10. Extension Points
- pluggable backoff strategy (exponential, linear, fixed, no-retry)
- future coroutine-based connect awaitable
- future connection timeout via EventLoop timer

---

## 11. Test Contracts
- direct non-owner start / stop / restart / retry mutation is rejected before
  Connector state is touched
- state snapshots remain race-free when observed from a non-owner thread
- start() initiates non-blocking connect on owner loop thread
- successful connect delivers fd through callback on owner loop thread
- the new-connection callback may synchronously trigger upper-layer connection
  teardown; previously queued TcpClient requests may run before deferred Channel
  destruction, so the completed attempt must no longer occupy Connector's slot
- a `ConnectSuccess` observer may synchronously call `restart()`; the new
  generation must retain its Channel and `kConnecting` state after the old
  callback frame returns, and the stale successful fd must not be published
- a `ConnectSuccess` observer may also call `stop()` followed by `start()` in
  the same frame; stop first settles the publishing state so start creates a
  real newer attempt rather than leaving `connect_=true` with no Channel
- connect failure triggers retry with configured backoff delay
- stop() during pending connect closes socket and removes Channel
- stop() during pending Windows ConnectEx cancels and drains the completion
  before releasing connector-owned Channel / operation storage
- Windows ConnectEx operation storage is retained by the IOCP Poller until its
  completion packet is dequeued. Timeout/stop cancellation keeps the Channel
  alive even when `CancelIoEx` reports that completion already won the race.
- stop() during pending retry cancels timer
- stop racing a due retry cannot publish a stale attempt
- restart after backoff growth restores the caller-configured initial delay
- `tests/contract/connector/test_connector_retry_stop.cpp` verifies stop()
  after retry scheduling prevents a later listener from receiving a stale retry
  connection
- `tests/contract/tcp_client/test_tcp_client_repeated_connect.cpp` verifies
  queued duplicate connect requests cannot collide with deferred destruction of
  the completed Connector Channel after the first connection tears down
- destroy during kConnecting state does not leak fd
- self-connect is detected and triggers retry
- recoverable pre-Channel socket setup failures leave no fd/Channel ownership
  behind and respect the configured retry policy
- `tests/contract/connector/test_connector_contract.cpp` verifies a throwing
  connector event observer cannot prevent successful connection establishment
- no callback fires after destruction
- `tests/contract/connector/test_connector_thread_contract.cpp` verifies the
  owner-only mutation boundary, atomic state snapshot, stop-versus-retry
  generation rejection, success-observer stop/restart/stop-then-start
  re-entry, configured restart delay, cross-thread TcpClient facade,
  terminal-failure observation, and queued-request target expiry
- Windows ConnectEx stop/completion retention remains covered by the existing
  pending-connect and mixed-timing TcpClient contracts; the deterministic
  retry-timer portions of the thread contract are platform-neutral and do not
  claim kernel-level ConnectEx race injection

---

## 12. Review Checklist
- Is Connector still a thin connect adapter without business logic?
- Is the connecting Channel removed on the correct thread?
- Are connect failures explicit rather than silently dropped?
- Can connected fds leak on callback absence, failure, or teardown?
- Is the state machine transitions complete and documented?
- Is backoff configuration explicit and not buried in implementation?
