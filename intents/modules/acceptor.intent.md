---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: Acceptor

## 1. Intent
Acceptor owns the listening socket registration for one EventLoop.
It converts readable listenfd events into accepted connection fds and
hands them upward through a narrow callback boundary.

---

## 2. Responsibilities
- own listen socket wrapper
- own listen Channel registration
- accept as many ready connections as possible per readable event
- on Windows, keep a fixed, configurable number of `AcceptEx` operations
  pre-posted so a connection burst does not serialize on one accept slot
- deliver accepted fds to upper layer on the loop thread
- report runtime accept/accepted-socket failures through an owner-loop policy
  callback with explicit Retry or Stop action
- stop accepting on explicit request (stop())

---

## 3. Non-Responsibilities
- does not create TcpConnection directly
- does not choose worker loop
- does not own TcpServer
- does not implement business protocol logic

---

## 4. Core Invariants
- Acceptor belongs to exactly one EventLoop
- listen Channel mutation happens only on owner loop thread
- accepted fds are either handed upward or closed explicitly
- destruction must not mutate Poller state from the wrong thread
- on IOCP, Acceptor owns one fixed pool on its base loop; each slot owns one
  accepted socket, `OVERLAPPED`, address buffer, and monotonically increasing
  submission generation, and a slot is never reused before its prior terminal
  completion is published to that owner loop
- the IOCP pre-post depth is configured before `listen()`, defaults to four,
  and is validated in the finite range `[1, 64]`; Linux retains drain-until-
  would-block behavior and does not allocate accept slots
- pending AcceptEx slot storage outlives every normal, error, or cancellation
  completion; stop marks each successfully submitted operation as one shutdown
  completion obligation, requests cancellation, revokes its Channel observer,
  and lets final drain release the retained fixed-pool state
- Retry is generation-wide: after one slot fails, every other submitted slot
  in that accept generation is canceled and consumed before the fixed pool is
  replenished
- a synchronous non-pending AcceptEx submission failure creates neither a
  storage lease nor a shutdown completion obligation
- process-level fixed-storage retention accounting begins when the Windows
  slot vector is allocated and ends only when the shared pool state is
  destroyed after every Poller lease is released; `stop()` cannot publish zero
  merely because Acceptor dropped its own shared pointer
- callback/error-policy and IOCP-depth configuration is owner-loop-only and is
  rejected while listening before any configured state is mutated

---

## 5. Threading Rules
- listen() is owner-thread only
- stop() is owner-thread only
- callback and option setters are owner-thread-only setup operations
- handleRead() runs on owner loop thread
- IOCP slot creation, submission, completion consumption, generation advance,
  cancellation, and reuse are owner-thread only
- error policy callbacks and retry timer callbacks run on the owner loop thread
- destructor must respect owner-thread teardown discipline

---

## 6. Failure Semantics
- accept interruption and would-block are handled explicitly
- listener construction/bind/listen failures throw a recoverable
  `std::system_error` to the caller instead of terminating the process
- unexpected accept, accepted-socket creation, and accepted-socket setup
  failures identify their stage and platform error code
- the default runtime action is a delayed retry; an installed policy may stop
  the Acceptor, and Retry disables readable interest until the retry timer to
  avoid an error spin loop
- on IOCP, Retry keeps the listen Channel available only to consume the
  canceled generation, waits for both the retry delay and zero submitted
  slots, and then replenishes the fixed pool
- teardown should not rely on accidental destructor side effects

---

## 7. Test Contracts
- listen() registers readable interest on owner loop
- accepted fd is forwarded through callback on loop thread
- no callback means accepted fd is closed explicitly
- destroy-before-listen path is safe
- stop() disables listening and removes Channel; idempotent when not listening
- destroying a stopped Acceptor while its EventLoop continues polling cannot
  expose freed operation or Channel storage to a late IOCP completion
- stopping and destroying an Acceptor in the same owner-loop callback as
  `EventLoop::quit()` keeps zero-timeout polling until the real AcceptEx
  cancellation packets for every submitted slot release all storage leases and
  shutdown obligations
- completion dispatch appends every exact AcceptEx operation identity from one
  IOCP batch to an allocation-free intrusive listen-Channel queue; one
  `handleRead()` drains that bounded queue, and a slot cannot be consumed or
  reused through another operation's completion
- callback re-entry through `stop()` is safe after the completed socket has
  left its slot, including when the replacement slot was already pre-posted
- `tests/contract/acceptor/test_acceptor_contract.cpp` verifies unavailable
  listener bind is reported by exception without process termination and the
  normal accept path does not spuriously invoke the runtime error policy
- `tests/contract/acceptor/test_acceptor_iocp_pool.cpp` verifies default and
  configured finite depth, burst replenishment, callback re-entry, generation-
  wide Retry after deterministic synchronous failure, delayed cancellation
  consumption, fixed-pool retained-byte visibility through the public process
  snapshot, and final zero slot/socket/completion/byte accounting
- `tests/integration/tcp/test_iocp_accept_connect_quit_completion_drain.cpp`
  verifies the multi-slot pending AcceptEx immediate-quit final-drain contract

---

## 8. Review Checklist
- Is Acceptor still a thin listenfd adapter?
- Is listen Channel removed on the correct thread?
- Are unexpected accept errors explicit?
- Can accepted fds leak on callback absence or teardown?
