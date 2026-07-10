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
- Channel mutation happens only on the owner loop thread
- at most one connect attempt is in flight at any time
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
- start() / stop() / restart() are owner-loop-thread-only operations
- TcpClient marshals cross-thread calls through runInLoop before invoking
  Connector methods
- handleWrite / handleError (Channel callbacks) run on the owner loop thread
- retry timer callback runs on the owner loop thread

---

## 7. Ownership Rules
- TcpClient owns Connector
- Connector owns the connecting socket fd until it is delivered upward
  or closed on failure
- Connector owns its Channel registration during the connect attempt
- Channel must be removed before Connector destruction

---

## 8. Failure Semantics
- connect refused / network unreachable / timeout:
  close the socket, invoke error notification, optionally schedule retry
- EINTR during connect: retry immediately
- getsockopt(SO_ERROR) non-zero after writable event: treat as connect failure
- self-connect detection (local addr == peer addr): close and retry
- destruction during pending connect: close socket, remove Channel, no callback
- destruction during pending retry timer: cancel timer, no callback

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

---

## 10. Extension Points
- pluggable backoff strategy (exponential, linear, fixed, no-retry)
- future coroutine-based connect awaitable
- future connection timeout via EventLoop timer

---

## 11. Test Contracts
- start() initiates non-blocking connect on owner loop thread
- successful connect delivers fd through callback on owner loop thread
- the new-connection callback may synchronously trigger upper-layer connection
  teardown; previously queued TcpClient requests may run before deferred Channel
  destruction, so the completed attempt must no longer occupy Connector's slot
- connect failure triggers retry with configured backoff delay
- stop() during pending connect closes socket and removes Channel
- stop() during pending Windows ConnectEx cancels and drains the completion
  before releasing connector-owned Channel / operation storage
- stop() during pending retry cancels timer
- `tests/contract/connector/test_connector_retry_stop.cpp` verifies stop()
  after retry scheduling prevents a later listener from receiving a stale retry
  connection
- `tests/contract/tcp_client/test_tcp_client_repeated_connect.cpp` verifies
  queued duplicate connect requests cannot collide with deferred destruction of
  the completed Connector Channel after the first connection tears down
- destroy during kConnecting state does not leak fd
- self-connect is detected and triggers retry
- no callback fires after destruction

---

## 12. Review Checklist
- Is Connector still a thin connect adapter without business logic?
- Is the connecting Channel removed on the correct thread?
- Are connect failures explicit rather than silently dropped?
- Can connected fds leak on callback absence, failure, or teardown?
- Is the state machine transitions complete and documented?
- Is backoff configuration explicit and not buried in implementation?
