---
status: active
target: GameNet::core
migration_source: native
promote_gate: none
artifact_kind: installed-library
---

# Architecture Intent: Owner-Loop I/O Engine

## 1. Intent

The I/O Engine boundary separates EventLoop scheduling from platform I/O
semantics without pretending that readiness and completion are the same model.
EventLoop remains the owner-thread scheduler and event pump. A source-private
Engine owns backend registration, submission, wait, cancellation observation,
and delivery of typed backend events to that EventLoop.

The first slice is an internal seam over the existing Poller implementation.
It must preserve public API, callback order, lifecycle, and performance before
the readiness and completion implementations diverge behind that seam.

## 2. Semantic Boundary

- Readiness reports that a generation-safe registration may make progress.
- Completion reports the terminal result of one identified operation.
- EventLoop schedules both, but it is not itself an epoll Reactor or an IOCP
  completion implementation.
- Channel is the readiness registration and callback binding. It is not the
  long-term carrier for completion-operation identity.
- Completion operation kind, identity, generation, storage lease, result,
  cancellation observation, and terminal retirement stay explicit.
- The shared abstraction is ownership, admission, bounded waiting, dispatch,
  and shutdown convergence; it is not a fake read/write event vocabulary.

## 3. Owner Thread and Cross-Thread Rules

One EventLoop owner thread owns one Engine instance. Only that thread may:

- register, modify, or remove readiness registrations;
- submit backend operations after loop-level admission;
- dispatch returned readiness or completion notices;
- retire registration generations or operation leases;
- advance the Engine lifecycle state.

External threads may only use existing EventLoop admission paths. They enqueue
work with `runInLoop`, `queueInLoop`, or a registered control source and wake
the loop. They never call the Engine directly. The seam therefore introduces
no second synchronization domain.

## 4. Ownership and Lifetime

- EventLoop uniquely owns and releases its Engine.
- A readiness registration observes a Channel owned by its higher-level
  subsystem. Removal invalidates the current generation before destruction.
- A successfully submitted completion operation retains its storage lease
  until the kernel packet is dequeued and terminally retired.
- Cancellation requests progress toward retirement; they do not consume the
  completion or authorize early storage release.
- Backend socket association and operation retention are Engine concerns, not
  application callback concerns.
- Shutdown reaches `Shutdown` only after accepted control work, registrations,
  and completion obligations have converged according to their model.

## 5. Re-entry and Dispatch

User callbacks can re-enter EventLoop APIs and can remove another registration,
close a connection, queue more work, or request quit. Engine wait never invokes
user code. EventLoop dispatches a bounded snapshot and revalidates registration
generation or completion identity before each callback. The current-dispatch
retirement slot continues to protect a Channel released by its own callback.

Wait and dispatch have separate budgets. A busy I/O source cannot indefinitely
starve timers, control sources, or pending functors, and a busy task queue cannot
prevent the loop from observing completion retirement.

## 6. Lifecycle and Failure Results

The Engine follows `Running -> Quiescing -> FinalDraining -> Shutdown` with the
owning EventLoop.

- Running admits normal registrations and operations.
- Quiescing rejects new external work while still observing accepted work.
- FinalDraining uses bounded nonblocking waits until registrations and
  completion obligations are retired.
- Shutdown owns no live backend registration or retained completion lease.

Submission and registration failures remain explicit and synchronous when no
kernel obligation was created. Once a completion operation was successfully
submitted, every outcome is delivered as exactly one terminal notice, including
cancellation and failure.

## 7. Compatibility Sequence

1. IOE-R1 introduces a source-private Engine contract and an adapter around the
   existing Poller. No new installed header, public API, or observable behavior
   change is introduced.
2. IOE-R2 moves epoll/kqueue-style registration into a Readiness Engine while
   preserving Channel contracts and generation invalidation.
3. IOE-C1 moves IOCP delivery to typed Completion notices and removes the fake
   Channel read/write translation.
4. Only proven, cross-backend concepts may later graduate to a narrow public
   capability surface. Platform-specific controls remain source-private.

## 8. Test Contracts

- `tests/contract/io_engine/test_io_engine_poller_adapter.cpp` verifies that the
  source-private adapter reports native backend capabilities, follows the
  EventLoop lifecycle, and dispatches a cross-thread wakeup on the owner loop.
- `tests/contract/poller/test_poller_contract.cpp` verifies bounded backend
  waiting and backend-neutral Poller behavior during the adapter slice.
- `tests/contract/event_loop/test_event_loop.cpp` verifies owner-thread dispatch,
  wakeup, admission, and shutdown behavior.
- `tests/contract/event_loop/test_event_loop_fair_budget.cpp` verifies that I/O,
  timer, control, and pending-functor work remain bounded and make progress.
- `tests/contract/channel/test_channel_active_batch_lifetime.cpp` verifies
  generation-safe remove-before-destroy behavior during re-entrant dispatch.
- `tests/contract/tcp_connection/test_tcp_connection_completion_drain.cpp`
  verifies completion-obligation drain before loop exit.
- `tests/integration/tcp/test_iocp_accept_connect_quit_completion_drain.cpp`
  verifies real AcceptEx/ConnectEx completion retention on Windows.

Every Engine implementation slice adds a focused contract before replacing a
backend path. Cross-platform full CTest and the core benchmark are required for
integration; a local sample is directional evidence, not a release gate.

## 9. Non-Goals

- no HTTP, WebSocket, RPC, UDP, KCP, TLS, or coroutine expansion;
- no public backend selection or platform handle API in IOE-R1;
- no unbounded queue introduced to hide pressure;
- no normalization of completion into synthetic readiness after IOE-C1;
- no new business logic in EventLoop or the Engine.

## 10. Review Questions

- Is every mutation still owned by the EventLoop thread?
- Does the change preserve remove-before-destroy or terminal completion
  retirement, as appropriate?
- Can a callback re-enter and invalidate the next dispatch item safely?
- Is every cross-thread path marshaled through EventLoop admission?
- Which focused contract fails if this semantic boundary regresses?
