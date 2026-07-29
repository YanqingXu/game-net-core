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
- expose the bounded Windows AcceptEx pre-post depth before start while leaving
  global/per-peer admission at the existing accepted-fd policy point
- create TcpConnection on chosen loop
- maintain connection map on base loop thread
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
- register one aggregate stop participant per worker loop so queue saturation
  admits at most O(worker-count) stop-control signals rather than one queued
  teardown task per connection
- coordinate the cross-loop release protocol
  `worker cleanup -> base bookkeeping -> BaseReleased -> worker ack -> join`
- propagate each connection's structured close result to server observers and
  admission/stop accounting

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
- backpressure configuration must not mutate worker-loop Channel state directly from base loop code
- shutdown should detach callbacks before asynchronous teardown continues
- stop() during active write must let connection-owner close/cancel ordering
  drain pending operations before connection destruction
- graceful-stop completion is published only after the final connection
  teardown and worker-loop join; timeout escalation remains single-shot
- admission options are immutable after start and default to disabled so
  existing servers preserve unlimited/admitted behavior
- AcceptEx pre-post depth is immutable after start; increasing transport-level
  accept concurrency does not bypass base-loop admission or transfer accepted-
  fd ownership before the existing admission decision
- every admission rejection happens while TcpServer still uniquely owns the
  accepted fd, which is then closed exactly once without creating TcpConnection
- active-per-peer counts, fixed-window rate buckets, and authentication timers
  are mutated only by the base loop and converge on removal/stop
- the peer rate table has an explicit finite capacity; an unseen peer is
  rejected rather than allowing abuse tracking itself to grow without bound
- each worker aggregate has one stop generation; repeated immediate/graceful
  requests coalesce and stale generations cannot affect a restarted server
- worker cleanup never waits synchronously for the base loop; it signals one
  base lifecycle node after its generation becomes locally quiet
- base bookkeeping erases/releases every matching connection before publishing
  `BaseReleased(generation)` to that worker
- a worker ack is published only after it observes BaseReleased, removes all
  server-owned Channels/callback links for the generation, and can no longer
  call the base TcpServer
- thread-pool quit/join starts only after every participating worker has acked;
  the graceful future becomes ready only after join
- normal and reserved pending-functor saturation cannot turn a committed
  graceful/immediate stop into SchedulingFailed; only pre-commit
  OwnerUnavailable/Shutdown can reject the request, with a defined terminal
  result and no partially-started stop

---

## 5. Threading Rules
- newConnection/removeConnectionInLoop run on base loop thread
- accept-error policy callbacks run on the base loop thread
- connectEstablished/connectDestroyed run on owning connection loop
- cross-loop handoff happens only through EventLoop scheduling APIs
- graceful-stop requests may originate on any thread but orchestration and
  connection-map decisions run on the base loop
- cross-thread stop admission uses the base loop lifecycle hub; per-worker
  aggregation uses each worker's lifecycle hub
- authentication completion may originate on a connection worker loop; the
  request is marshaled to the base loop before deadline state is mutated
- admission metric callbacks run on the base loop and their exceptions are
  logged and contained

---

## 6. Failure Semantics
- worker-loop teardown should not leave stale entries in the base-loop map
- shutdown should tolerate already-closing connections
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
- an aggregate worker stop signal that returns Accepted must eventually reach
  worker ack or a documented owner-unavailable rollback; it cannot be dropped
  because the normal queue is full
- `BaseReleased` is generation tagged and a stale release/ack is a no-op
- connection close reasons are preserved through worker cleanup and base map
  removal; shutdown escalation cannot overwrite an earlier peer/error reason

---

## 7. Test Contracts
- new connections are assigned to a loop and registered there
- close callback removes the connection through the base loop
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
- `tests/contract/tcp_server/test_tcp_server_saturation_shutdown.cpp` fills
  normal and reserved queues on the base and every worker, exceeds ordinary
  queue capacity with live connections, and proves the O(worker-count)
  aggregate stop, BaseReleased/worker-ack handshake, empty admission state,
  join, and future completion
- `tests/contract/tcp_server/test_tcp_server_release_handshake.cpp` verifies
  generation-tagged worker cleanup, base release before Channel destruction,
  stale ack rejection, callback re-entry, and exact-once join

---

## 8. Deferred General Idle Policy

The active API implements only `unauthenticatedTimeout`: a base-loop admission
deadline that is canceled when the accepted connection is explicitly marked
authenticated. It does not implement a general connection idle timeout, and
this intent must not present one as current behavior.

A scalable general idle policy is deferred to the M3 deadline-bucket/time-wheel
work. Its first promoted form should be read-idle and must define:

- activity timestamps updated by the TcpConnection owner loop;
- generation-tagged deadlines so stale expiry work cannot close a replacement
  connection;
- convergence through the normal close/remove and structured-reason path;
- ordering against peer close, force close, graceful stop, authentication
  deadline, heartbeat, and an optional explicit activity refresh;
- bounded 10k/100k deadline-storm memory and loop-lag evidence rather than one
  allocation-heavy timer object per connection.

---

## 9. Review Checklist
- Does any callback still capture raw TcpServer lifetime unsafely?
- Is base-loop bookkeeping isolated from worker-loop teardown?
- Are shutdown and connection removal still explicit and predictable?
- Is the unauthenticated admission deadline kept distinct from the deferred
  general read-idle policy?
- Can any rejected attempt or repeated stop leave a live authentication timer,
  peer count, or unbounded peer-rate record behind?
