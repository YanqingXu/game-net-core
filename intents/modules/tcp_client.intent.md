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
- deliver connection / message / close / write-complete callbacks to the user
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
- Connector Channel and TcpConnection Channel are removed before
  effective destruction
- reconnect policy is explicit and configurable, never silently hardcoded

---

## 5. Collaboration
- uses Connector to perform non-blocking connect and detect success/failure
- creates TcpConnection on the owner loop after Connector delivers a connected fd
- TcpConnection uses the same owner loop as TcpClient
- may use TimerQueue (through EventLoop) for reconnect backoff delays
- user interacts with TcpClient through callbacks set before connect()

---

## 6. Threading Rules
- connect() and disconnect() may be called cross-thread;
  they must marshal into the owner loop via runInLoop
- newConnection / removeConnection run on the owner loop thread
- user callbacks (connection / message / close) fire on the owner loop thread
- Connector state machine transitions happen on the owner loop thread only

---

## 7. Ownership Rules
- TcpClient owns Connector (unique_ptr or scoped member)
- TcpClient holds TcpConnection via shared_ptr after connect succeeds
- TcpConnection close callback must prevent use-after-free of TcpClient
  (e.g. via weak capture or explicit guard, same discipline as TcpServer)
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

---

## 9. Extension Points
- pluggable retry/backoff policy (e.g. exponential, fixed, no-retry)
- future coroutine-based connect awaitable
  (e.g. `co_await client.asyncConnect()`)
- future TLS handshake integration after TCP connect succeeds

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
- `tests/contract/tcp_client/test_tcp_client_cross_thread_connect.cpp`
  verifies non-owner connect() marshals Connector startup to the owner loop and
  publishes connection callbacks on that loop
- cross-thread connect marshals to owner loop before Connector starts
- cross-thread disconnect marshals to owner loop before teardown begins
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
