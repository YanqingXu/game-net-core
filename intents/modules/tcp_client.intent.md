---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: TcpClient

## 1. Intent
TcpClient is the client-side counterpart to TcpServer.
It coordinates active connection establishment through Connector,
manages the resulting TcpConnection, and provides reconnect capability —
all while preserving the same owner-loop discipline as the server side.

---

## 2. Responsibilities
- own one Connector for active connection initiation
- own one TcpConnection (shared_ptr) after connect succeeds
- expose connect / disconnect APIs that respect owner-loop threading
- expose a copyable `TcpClientControl` handle whose storage is independent of
  TcpClient lifetime and whose requests use the owner EventLoop lifecycle hub
- deliver connection / message / close / write-complete callbacks to the user
- propagate the established connection's immutable structured close result to
  a client observer on the owner loop
- support configurable retry-on-failure and reconnect-on-disconnect policies
- coordinate safe teardown of Connector and TcpConnection on destruction

---

## 3. Non-Responsibilities
- does not own EventLoop
- does not perform per-connection I/O directly (delegated to TcpConnection)
- does not implement application protocol logic
- does not manage multiple simultaneous connections (one client = one connection)
- does not implement DNS resolution

---

## 4. Core Invariants
- one TcpClient belongs to exactly one EventLoop
- connection state mutation happens on the owner loop thread only
- at most one active Connector attempt or one established TcpConnection
  exists at any time
- repeated `connect()` submissions for one pending or active connection
  lifecycle are coalesced before owner-loop dispatch
- each accepted cross-thread connect / disconnect / stop request carries a
  monotonically increasing facade generation; only the latest generation may
  mutate Connector or TcpConnection intent on the owner loop
- an explicit connect accepted after disconnect has already moved the current
  connection out of `kConnected` is retained as a pending reconnect request;
  it is distinct from automatic retry policy and starts after the old
  connection's owner-loop removal even when retry is disabled
- Connector Channel and TcpConnection Channel are removed before
  effective destruction
- reconnect policy is explicit and configurable, never silently hardcoded
- `TcpClientControl` never owns TcpClient; its mailbox owns only the
  generation-tagged requested operation and a non-owning lifecycle source
- destroying TcpClient closes the control mailbox and detaches its lifecycle
  node on the owner loop before releasing client storage
- a control request returning Accepted is committed independently of pending-
  functor saturation and is processed once or superseded by a newer generation
- a handle call after mailbox closure returns OwnerUnavailable without reading
  TcpClient storage

---

## 5. Collaboration
- uses Connector to perform non-blocking connect and detect success/failure
- creates TcpConnection on the owner loop after Connector delivers a connected fd
- TcpConnection uses the same owner loop as TcpClient
- may use TimerQueue (through EventLoop) for reconnect backoff delays
- user interacts with TcpClient through callbacks set before connect()
- the lifecycle-node callback is the only path from independent control
  storage back into live TcpClient state

---

## 6. Threading Rules
- connect() and disconnect() may be called cross-thread; non-owner calls
  marshal through the cached EventLoopExecutor
- typed `tryConnect()` / `tryDisconnect()` / `tryStop()` facade entry points
  return `PostResult`: Accepted only after owner-loop admission, QueueFull on
  bounded queue rejection, Shutdown after admission seals, and
  OwnerUnavailable when the cached owner executor has expired
- legacy void connect / disconnect / stop remain compatibility wrappers over
  typed admission; code that needs overload or shutdown correctness uses the
  typed entry points
- connect request admission uses an atomic request id; Connector and
  TcpConnection state still mutate only on the owner loop
- accepted connect / disconnect / stop lambdas compare their captured facade
  generation with the latest requested generation before touching loop-owned
  state, so enqueue order cannot resurrect a superseded request
- an accepting owner-thread call preserves runInLoop-style inline semantics:
  admission state is published under the facade mutex, the mutex is released,
  and only then may Connector, TcpConnection, or a user callback execute
- enableRetry() and disableRetry() remain compatibility wrappers. Typed
  `tryEnableRetry()` / `tryDisableRetry()` may be called cross-thread, return
  QueueFull/Shutdown/OwnerUnavailable distinctly, and update Connector retry
  state only on the owner loop
- callback setters are owner-loop-only. They configure future callback
  publication; connection backpressure options must additionally be set before
  an accepted connect lifecycle
- newConnection / removeConnection run on the owner loop thread
- user callbacks (connection / message / close) fire on the owner loop thread
- Connector state machine transitions happen on the owner loop thread only
- `TcpClientControl::tryConnect/tryDisconnect/tryStop` may run on any thread;
  they update only the shared mailbox and signal its lifecycle source
- the lifecycle callback snapshots the newest operation, releases the mailbox
  lock, and only then invokes the live TcpClient facade on the owner loop

---

## 7. Ownership Rules
- TcpClient owns Connector (unique_ptr or scoped member)
- TcpClient holds TcpConnection via shared_ptr after connect succeeds
- TcpConnection close callback must prevent use-after-free of TcpClient
  (e.g. via weak capture or explicit guard, same discipline as TcpServer)
- TcpClient owns the control mailbox while alive, but copied
  `TcpClientControl` handles may keep the closed mailbox storage alive after
  TcpClient destruction; neither mailbox nor handle owns TcpClient/EventLoop
- destruction must run on the owner loop thread to safely clean up
  Channel registrations

---

## 8. Failure Semantics
- connect failure (refused, timeout, network unreachable) is reported
  explicitly through a connection callback with appropriate state
- retry after failure uses the configured backoff strategy via EventLoop timer
- stop() cancels pending retry or connect work so a later server start cannot
  resurrect a stopped client through a stale timer
- stop() during an in-flight ConnectEx cancels and drains the connector-owned
  operation before releasing Channel storage
- disconnect during an active connect attempt cancels Connector cleanly
- destruction during a pending connect or reconnect timer must not leak
  fds or leave stale Channel registrations
- repeated disconnect calls are idempotent
- if a newer connect supersedes a disconnect before disconnect executes, the
  still-connected TcpConnection is rebound to that request; if disconnect has
  already begun, the request is retained separately and cannot be cleared by
  the old connection's close callback
- a terminal connect failure without a scheduled retry releases the admitted
  request so a later explicit `connect()` can start a fresh attempt
- a terminal connect failure is observable through a dedicated owner-loop
  callback carrying the Connector failure event; admission is released before
  that callback runs so it may synchronously request a new lifecycle
- terminal-failure observer exceptions are logged and contained
- Connector transfers a connected fd at new-connection callback entry. The
  receiver immediately places it under RAII before fallible work; Connector
  never retains a second fd owner across that handoff
- on Windows, address inspection and TcpConnection construction complete
  before IOCP association handoff begins; association preservation,
  replacement Channel registration, client publication, and request
  bookkeeping form one rollback-capable transaction
- if association preservation or replacement Channel registration throws,
  TcpClient removes any provisional connection publication, invalidates the
  numeric association record, closes the transferred fd, releases the active
  request as a terminal connect failure, and lets Connector observe callback
  failure instead of treating the handoff as successful
- an established connection's disconnected callback runs before the internal
  close callback removes it from TcpClient; an explicit `tryConnect()` from
  that disconnected callback is a new request, not a repeated request for the
  already-ended lifecycle, and must survive the subsequent removal
- a queued facade request whose TcpClient lifetime token has expired is
  discarded without dereferencing the destroyed client
- typed admission rejection rolls back any connect request id reserved by that
  call; a QueueFull / Shutdown / OwnerUnavailable result cannot leave the
  facade permanently coalescing later explicit connect requests
- EventLoop lifetime still strictly encloses TcpClient lifetime. Calling a
  facade after its EventLoop has been destroyed is outside the ownership
  contract and is not reclassified as a supported "owner unavailable" path
- callback-exception observation is propagated to the active TcpConnection;
  business callback failure follows the same single-connection close policy as
  server-owned connections
- established close observers receive the same first-wins
  `TcpConnectionCloseInfo` that TcpConnection publishes; reconnect/stop cannot
  overwrite an earlier peer or transport error
- a stale control generation or a signal racing mailbox closure performs no
  Connector, socket, or callback work

---

## 9. Extension Points
- pluggable retry/backoff policy (e.g. exponential, fixed, no-retry)
- future coroutine-based connect awaitable
  (e.g. `co_await client.asyncConnect()`)
- future TLS handshake integration after TCP connect succeeds

Connector policy is exposed at TcpClient construction through
`ConnectorOptions`, including initial/max retry delay, connect timeout, and
initial retry enablement. Configuration is fixed before the first attempt;
runtime retry enable/disable remains marshaled through the facade.

---

## 10. Test Contracts
- connect to a listening server establishes TcpConnection on owner loop thread
- connect to a refused port reports failure through connection callback
- disconnect cancels pending connect and cleans up Channel registration
- reconnect after server-initiated close uses configured backoff delay
- `tests/contract/tcp_client/test_tcp_client_retry_stop_race.cpp` verifies
  stop() cancels pending retry before a later server start can connect
- `tests/contract/tcp_client/test_tcp_client_retry_stop_soak.cpp` repeats the
  retry-stop race to catch stale retry timers across multiple client lifecycles
- `tests/contract/tcp_client/test_tcp_client_stop_pending_connect.cpp` verifies
  stop() cancels an in-flight ConnectEx before a later server start can connect
- `tests/contract/tcp_client/test_tcp_client_stop_pending_connect_soak.cpp`
  repeats stop() during in-flight ConnectEx attempts to catch stale completion
  resurrection across multiple client lifecycles
- `tests/contract/tcp_client/test_tcp_client_cross_thread_stop_pending_connect.cpp`
  verifies non-owner stop() marshals cancellation of an in-flight ConnectEx to
  the owner loop before a later server start can publish a stopped connection
- `tests/contract/tcp_client/test_tcp_client_stop_pending_connect_mixed_timing_soak.cpp`
  alternates immediate and delayed owner/non-owner stop() timing during pending
  ConnectEx attempts to catch stale completion resurrection under varied timing
- `tests/contract/tcp_client/test_tcp_client_destroy_pending_connect.cpp`
  verifies owner-loop destruction during a pending ConnectEx clears callbacks
  and cancels connector work before a later server start can resurrect a client
- `tests/contract/tcp_client/test_tcp_client_destroy_active_connection.cpp`
  verifies owner-loop destruction with an active TcpConnection releases client
  ownership and lets peer teardown converge without stale TcpClient callbacks
- `tests/contract/tcp_client/test_tcp_client_stop_active_connection_mixed_timing_soak.cpp`
  verifies immediate and delayed owner/non-owner stop() timing on an active
  retry-enabled connection clears future connect intent before peer close can
  resurrect the client through retry
- `tests/contract/tcp_client/test_tcp_client_cross_thread_disconnect_active.cpp`
  verifies non-owner disconnect() on an active connection marshals graceful
  teardown to the owner loop and converges through normal close/remove paths
- `tests/contract/tcp_client/test_tcp_client_repeated_disconnect.cpp`
  verifies repeated owner and non-owner disconnect() calls are idempotent and
  converge through one client/server disconnect callback pair
- `tests/contract/tcp_client/test_tcp_client_repeated_stop.cpp`
  verifies repeated owner and non-owner stop() calls on an active retry-enabled
  client are idempotent and prevent peer close from resurrecting the client
- `tests/contract/tcp_client/test_tcp_client_repeated_connect.cpp`
  verifies repeated owner and non-owner connect() calls are idempotent, create
  at most one active client/server connection pair, and do not prevent a fresh
  explicit connect after a terminal no-retry failure
- `tests/contract/tcp_client/test_tcp_client_cross_thread_connect.cpp`
  verifies non-owner connect() marshals Connector startup to the owner loop and
  publishes connection callbacks on that loop
- `tests/contract/tcp_client/test_tcp_client_cross_thread_retry_config.cpp`
  verifies non-owner `tryDisableRetry()` marshals retry-state mutation to the owner
  loop and prevents peer close from resurrecting a disabled-retry client
- cross-thread connect marshals to owner loop before Connector starts
- cross-thread disconnect marshals to owner loop before teardown begins
- cross-thread retry configuration marshals to owner loop before Connector
  retry state changes
- constructor-supplied ConnectorOptions control retry/backoff and timeout
- terminal no-retry failure is published once on the owner loop and permits a
  re-entrant explicit connect
- latest facade generation wins across queued connect-versus-stop requests
- owner-loop `disconnect()` followed immediately by `connect()` while the old
  connection is closing establishes a second connection with retry disabled;
  the old close callback cannot consume the newer admitted request
- retry-disabled peer close followed by owner-loop `tryConnect()` from the
  disconnected callback establishes a second connection; returning Accepted
  is forbidden unless that explicit request is retained
- deterministic Windows faults at IOCP association preservation and
  replacement Channel registration both roll back provisional publication,
  association bookkeeping, fd ownership, and active request admission before
  a re-entrant explicit connect is attempted
- typed facade admission distinguishes queue saturation, sealed shutdown, and
  unavailable owner instead of collapsing them to a boolean
- `tests/contract/tcp_client/test_tcp_client_control_lifetime.cpp` verifies
  queue-saturation admission, latest-operation coalescing, owner-loop
  execution, lifecycle detach, and OwnerUnavailable after TcpClient destruction
- `tests/contract/tcp_client/test_tcp_client_close_reason.cpp` verifies client
  close-info propagation and first-close-reason preservation
- a queued request observes an expired TcpClient target and performs no
  callback or socket work
- `tests/contract/connector/test_connector_thread_contract.cpp` is the direct
  cross-module contract for these Connector/TcpClient thread boundaries
- destruction during pending connect does not leak fd or crash
- destruction with an active TcpConnection cleans up safely

---

## 11. Review Checklist
- Is all mutable client state still owner-loop-owned?
- Can callbacks outlive TcpClient safely?
- Is Connector Channel removed before TcpClient destruction completes?
- Is TcpConnection Channel removed before TcpClient destruction completes?
- Is reconnect policy explicit and not buried in implementation details?
- Does cross-thread connect/disconnect use runInLoop consistently?
