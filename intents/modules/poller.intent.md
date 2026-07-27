---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: Poller

## 1. Intent
Poller is the I/O multiplexing abstraction used by EventLoop.
Its role is to wait for I/O readiness from the backend and translate that
into a stable list of active Channel objects for EventLoop dispatch.

Poller is not business logic.
Poller is not callback dispatch logic.
Poller is not channel owner.

---

## 2. Responsibilities
- maintain backend registration for Channel objects
- wait for active I/O events
- fill active channel collection for EventLoop
- keep registration bookkeeping consistent
- hide backend-specific details behind stable interface
- retain opaque completion-operation storage when an async backend can deliver
  a cancellation packet after its upper adapter has removed the Channel

---

## 3. Non-Responsibilities
- does not own Channel
- does not use retained operation storage to extend Channel or application
  object lifetime; canceled operations clear their Channel observer first
- does not own EventLoop
- does not invoke business callbacks directly
- does not define connection lifecycle
- does not decide thread-pool scheduling

---

## 4. Core Invariants
- one Poller belongs to one EventLoop
- Poller is only used by owner EventLoop thread
- backend registration state must be consistent with internal channel bookkeeping
- removed channels must not continue appearing as valid active channels
- one poll result contains at most one active entry per Channel; backend event
  bits are merged before EventLoop dispatch
- backend removal validates both fd and Channel identity before erasing
  bookkeeping, so a stale same-fd remove cannot erase a replacement Channel
- IOCP completion metadata remains allocated until the completion is dequeued
  or the owning Poller closes; late canceled completions observe no Channel
- IOCP only activates a Channel for a packet actually dequeued from the
  completion port; a synchronous overlapped-submission failure is returned to
  the posting owner and is never represented by fabricated `revents`
- IOCP numeric-socket association bookkeeping tracks either a currently
  registered Channel or an immediately consumed same-loop TcpClient handoff;
  closing/rejecting a raw Connector handoff before Channel registration must
  not leave a stale numeric SOCKET entry that can poison later handle reuse
- association preservation is provisional until the replacement Channel
  registers; failed TcpClient publication explicitly forgets the numeric
  association record before the SOCKET value can be reused
- one dequeued normal, error, or cancellation packet publishes one terminal
  result into its observed operation record
- backend errors must not silently corrupt Poller internal state

---

## 5. Collaboration
- EventLoop owns Poller and calls poll/update/remove
- Channel provides fd and event masks
- concrete backend implementation (e.g. epoll/IOCP under `src/core/net/poller/`) performs actual OS interaction

---

## 6. Interface Direction
Typical interface direction:
- poll(timeoutMs, activeChannels)
- updateChannel(Channel*)
- removeChannel(Channel*)
- hasChannel(Channel*) optional in debug path

Concrete backend implementations may extend internal helpers but should preserve public semantic contract.

The returned `Channel*` list is a non-owning readiness snapshot. EventLoop
assigns active-batch membership before invoking callbacks and invalidates a
removed Channel's pending slot before successful remove returns. Poller neither
owns nor extends Channel lifetime.

---

## 7. Threading Rules
- Poller methods are owner-thread only
- no direct cross-thread backend mutation
- backend wakeup integration is coordinated via EventLoop mechanisms

---

## 8. Backend Abstraction Intent
v1 backend targets are Linux epoll and Windows IOCP.
Poller abstraction should preserve the possibility of:
- kqueue backend
- io_uring-related future experiment
without forcing upper layers to change semantic assumptions.

Windows WinSock `select()` is not an acceptable performance target for the
current migration. It may remain only as deleted legacy context or local
experimentation outside the active target; CI must validate the IOCP path before
Windows support is promoted. See `docs/development/windows_iocp_milestone.md`
for the required IOCP completion ownership and promotion gates.

---

## 9. Failure Semantics
Poller should distinguish:
- backend wait interruption
- backend registration failure
- invalid removal/update path
- stale channel state bug

Handling can be assert/log/fail-fast depending on severity, but must remain explicit.

---

## 10. Risk Areas
High-risk mistakes:
- internal channel map diverges from backend registration
- remove path leaves stale backend state
- active event fill produces invalid Channel pointers
- backend event translation leaks too much backend-specific complexity upward
- non-owner-thread access slips in unnoticed

---

## 11. Test Contracts
- updateChannel registers or updates backend state correctly
- removeChannel unregisters correctly
- poll returns active channels accurately
- invalid path is guarded or diagnosed
- internal bookkeeping remains consistent after repeated updates
- repeated removal and same-fd replacement cannot diverge backend registration
  from the internal Channel map
- a deterministic multi-entry dispatch harness verifies stale active entries
  are skipped on both platforms even while the current IOCP backend dequeues
  one completion per poll call
- `tests/contract/acceptor/test_acceptor_contract.cpp` destroys a stopped
  Acceptor while the EventLoop continues polling and guards late IOCP completion
- `tests/contract/connector/test_connector_retry_stop.cpp` covers ConnectEx
  timeout/cancel with a delayed completion packet; full MSVC ASan verifies the
  retained operation cannot observe a released Channel
- `tests/contract/connector/test_connector_thread_contract.cpp` verifies a
  callback-rejected connected fd leaves no preserved IOCP association after
  callback unwinding
- the same contract deterministically injects association-preserve and
  replacement-registration failures and verifies rollback plus a fresh
  explicit connect
- `tests/contract/tcp_connection/test_tcp_connection_iocp_sync_error.cpp`
  distinguishes synchronous submit failure from a real queued cancellation
  completion and verifies both converge exactly once through TcpConnection

---

## 12. Review Checklist
- Is Poller accidentally owning Channel?
- Is backend state synchronized with bookkeeping?
- Are owner-thread assumptions enforced?
- Are error paths explicit enough?
- Is EventLoop-facing interface stable and backend-neutral enough?
