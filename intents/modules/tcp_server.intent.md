---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: TcpServer

## 1. Intent
TcpServer coordinates accept, worker-loop selection, and connection bookkeeping.
It is the lifecycle boundary between listening infrastructure and per-connection objects.

---

## 2. Responsibilities
- own Acceptor and EventLoopThreadPool collaboration
- create TcpConnection on chosen loop
- maintain connection map on base loop thread
- optionally coordinate per-connection idle-timeout policy through owner-loop timers
- optionally install per-connection backpressure thresholds for accepted connections
- expose the Acceptor runtime-error policy and apply its Retry/Stop decision to
  accepted-socket setup failures before connection ownership is established
- install one diagnostic callback-exception observer on every accepted
  connection while keeping the fixed per-connection isolation policy
- optionally enforce finite global/per-peer connection counts and a bounded
  per-peer fixed-window connection-attempt rate before constructing TcpConnection
- optionally require accepted connections to be explicitly marked authenticated
  before a base-loop-owned deadline closes them
- expose cumulative admission counters and base-loop decision metrics without
  retaining an unbounded set of peer addresses
- remove connections safely during close/shutdown
- orchestrate ordered shutdown via stop()
- expose completion-aware graceful stop with bounded drain time and explicit
  drained/forced terminal results

---

## 3. Non-Responsibilities
- does not perform per-connection I/O itself
- does not own worker EventLoop objects directly beyond thread-pool coordination
- does not process application protocol payloads
- does not authenticate credentials or define account, ban-list, or protocol
  semantics; it only provides the transport admission/deadline mechanism

---

## 4. Core Invariants
- base loop owns connection map mutation
- close/remove path must not dereference a destroyed TcpServer
- connection creation and removal remain explicit and loop-safe
- idle-timeout policy must not bypass connection owner-loop close semantics
- backpressure configuration must not mutate worker-loop Channel state directly from base loop code
- shutdown should detach callbacks before asynchronous teardown continues
- stop() during active write must let connection-owner close/cancel ordering
  drain pending operations before connection destruction
- graceful-stop completion is published only after the final connection
  teardown and worker-loop join; timeout escalation remains single-shot
- admission options are immutable after start and default to disabled so
  existing servers preserve unlimited/admitted behavior
- every admission rejection happens while TcpServer still uniquely owns the
  accepted fd, which is then closed exactly once without creating TcpConnection
- active-per-peer counts, fixed-window rate buckets, and authentication timers
  are mutated only by the base loop and converge on removal/stop
- the peer rate table has an explicit finite capacity; an unseen peer is
  rejected rather than allowing abuse tracking itself to grow without bound

---

## 5. Threading Rules
- newConnection/removeConnectionInLoop run on base loop thread
- accept-error policy callbacks run on the base loop thread
- connectEstablished/connectDestroyed run on owning connection loop
- cross-loop handoff happens only through EventLoop scheduling APIs
- graceful-stop requests may originate on any thread but orchestration and
  connection-map decisions run on the base loop
- authentication completion may originate on a connection worker loop; the
  request is marshaled to the base loop before deadline state is mutated
- admission metric callbacks run on the base loop and their exceptions are
  logged and contained

---

## 6. Failure Semantics
- worker-loop teardown should not leave stale entries in the base-loop map
- shutdown should tolerate already-closing connections
- idle timeout should converge on the normal connection close/remove path
- backpressure configuration should not leave accepted connections permanently read-paused after drain
- callback lifetime must remain safe during server destruction
- drain timeout, immediate-stop escalation, scheduling failure, and server
  destruction produce explicit completion outcomes
- recoverable accept and accepted-socket setup failures close any untransferred
  fd and either continue admission or stop the server according to policy
- a business callback exception closes only the offending connection; server
  connection-map removal and later admission remain available
- global, per-peer, rate, peer-tracking-capacity, and authentication-timeout
  rejections have distinct counters/events; disabled limits reject nothing
- if authentication completion and its deadline race, whichever base-loop task
  executes first wins, and the later task is a no-op
- stopping or removing a connection cancels its authentication timer and
  releases active-per-peer accounting exactly once

---

## 7. Test Contracts
- new connections are assigned to a loop and registered there
- close callback removes the connection through the base loop
- idle timeout closes quiet connections without skipping the normal removal path
- backpressure policy installed by TcpServer pauses and later resumes per-connection reads without breaking ownership rules
- destruction invalidates delayed removal callbacks safely
- stop() stops Acceptor, force-closes all connections, stops thread pool; idempotent
- `tests/contract/tcp_server/test_tcp_server_stop_active_write.cpp` verifies
  immediate stop during an active write plus completion-aware graceful output
  drain, repeated future observation, timeout force-close, and exact result counts
- `tests/contract/tcp_server/test_tcp_server_stop_soak.cpp` repeats stop()
  from worker-owned connections and verifies worker-loop teardown completes
  before the thread pool is joined
- `tests/contract/tcp_server/test_tcp_server_stop_multi_worker.cpp` verifies
  base-loop stop() drains multiple worker-owned connections and leaves base-loop
  bookkeeping empty after teardown converges
- `tests/contract/tcp_server/test_tcp_server_stop_worker_active_write_soak.cpp`
  repeats base-loop stop() while worker-owned connections have active writes and
  verifies owner-loop teardown drains before base-loop bookkeeping reaches zero
- `tests/contract/tcp_server/test_tcp_server_stop_from_worker_callback_soak.cpp`
  repeats stop() re-entering from a worker-owned connection callback with
  multiple worker-owned connections and verifies base-loop bookkeeping still
  drains to zero
- `tests/contract/tcp_server/test_tcp_server_repeated_stop.cpp` verifies
  repeated base-loop stop() requests while a worker-owned connection is active
  remain idempotent and still converge on a single disconnect callback
- `tests/contract/tcp_server/test_tcp_server_contract.cpp` verifies
  server-level observer propagation, per-connection isolation, disconnect-
  callback containment, continued admission after the failure, finite global
  and per-peer admission, fixed-window rate rejection, bounded peer tracking,
  cross-thread authentication completion, and unauthenticated deadline close

---

## 8. Review Checklist
- Does any callback still capture raw TcpServer lifetime unsafely?
- Is base-loop bookkeeping isolated from worker-loop teardown?
- Are shutdown and connection removal still explicit and predictable?
- Can any rejected attempt or repeated stop leave a live authentication timer,
  peer count, or unbounded peer-rate record behind?
