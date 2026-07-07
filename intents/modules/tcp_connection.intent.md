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
- translate read/write/close/error events into unified connection state changes
- expose send/shutdown APIs that preserve owner-thread execution
- expose force-close style teardown entry that still converges on the same close path
- expose connection context as loop-owned mutable state, not cross-thread storage
- dispatch user-visible connection/message/write-complete/high-water/close callbacks
- coordinate loop-owned helper components for transport, coroutine waiting,
  and backpressure without bypassing EventLoop scheduling

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
- close and error teardown converge on one safe path
- channel registration is removed before effective destruction
- helper component state that mutates connection behavior is still owned by the same loop
- backpressure-driven read suspend/resume only changes Channel interest on owner loop thread
- coroutine resume returns through EventLoop scheduling
- transport-specific behavior (plain TCP or TLS) must not change callback ordering
  or close-path convergence

---

## 5. Collaboration
- TcpConnection owns Socket, Channel, input/output Buffer, and callback slots
- TcpConnection may delegate read/write/shutdown details to ConnectionTransport
- TcpConnection may delegate coroutine waiter state to ConnectionAwaiterRegistry
- TcpConnection may delegate threshold-based read pause/resume to
  ConnectionBackpressureController
- TcpConnection remains the only component that changes the public connection state
  or invokes the final close callback used by TcpServer/TcpClient

---

## 6. Threading Rules
- handleRead/handleWrite/handleClose/handleError run on owner loop thread
- cross-thread send/shutdown must marshal back into the loop
- setContext/getContext are owner-loop-only; cross-thread users must marshal
  context access through `runInLoop()` or `queueInLoop()`
- helper components must not create a second mutable thread domain
- backpressure threshold evaluation and read-interest toggling happen on the owner loop
- await readiness checks must not inspect loop-owned mutable state off-thread
- transport handshake/read/write/shutdown logic runs on the owner loop thread only

---

## 7. Failure Semantics
- fatal read/write errors should produce explicit teardown
- repeated close/error handling should be guarded or idempotent
- error-triggered teardown remains single-shot even if user code re-enters a
  teardown API from the close callback
- pending IOCP read/write operations are canceled and drained on the owner loop
  before force-close teardown releases connection-owned operation storage
- timeout-driven close should reuse the normal close path rather than inventing a side channel
- backpressure throttling should resume automatically after the output buffer drains below the low-water threshold
- disconnected state should block unsafe user-visible actions
- shutdown waits for pending output to drain before issuing the socket
  half-close
- high-water callback fires once when output crosses the threshold and is
  delivered on the owner loop
- helper-component failure must still converge on TcpConnection's existing error/close model

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
- `tests/contract/tcp_connection/test_tcp_connection_shutdown_pending_output.cpp`
  verifies shutdown waits for pending output to drain before peer EOF
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_shutdown.cpp`
  verifies cross-thread shutdown marshals to the owner loop, drains pending
  output, and then half-closes exactly once
- `tests/contract/tcp_connection/test_tcp_connection_high_water_mark.cpp`
  verifies high-water callback threshold delivery on the owner loop
- `tests/contract/tcp_connection/test_tcp_connection_repeated_force_close.cpp`
  verifies repeated forceClose teardown remains single-shot
- `tests/contract/tcp_connection/test_tcp_connection_cross_thread_force_close_soak.cpp`
  repeats cross-thread forceClose and verifies teardown marshals to the owner
  loop without duplicating callbacks
- `tests/contract/tcp_connection/test_tcp_connection_force_close_pending_read.cpp`
  verifies forceClose cancels a pending read and waits for the owner-loop
  completion path before connection destruction
- high-water to low-water drain path pauses and resumes read processing on the owner loop
- coroutine awaiters resume through EventLoop rather than arbitrary caller thread
- repeated teardown does not leave stale registration behind
- swapping transport implementation does not change connection callback ordering
  or teardown semantics

---

## 10. Review Checklist
- Is all mutable connection state still loop-owned?
- Do close and error share one teardown model?
- Can callbacks or coroutine resumes outlive safe ownership?
- Is registration removed before destruction?
- Are optional behaviors delegated cleanly without weakening the public lifecycle model?
