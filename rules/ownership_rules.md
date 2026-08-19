# ownership_rules.md

## 1. Core Principle
Ownership must be explicit.
A module should either:
- own an object
- borrow an object
- observe an object
It must not blur these roles.

## 2. EventLoop
- EventLoop owns its Poller
- EventLoop owns wakeup-related resources it creates
- On Windows, the owned Poller exclusively owns the wakeup-pending state and
  completion-port handle. Producers own neither; a successful false-to-true
  transition grants only the obligation to post one signal packet before
  returning
- EventLoop owns the fixed control-source slot table, pending bitset, and
  registered callback storage
- EventLoop owns its dynamic lifecycle hub, attached-node registry, intrusive
  dirty links, generation counters, and callback storage
- EventLoop does not own business-layer connection semantics
- EventLoop may temporarily own one already-removed current Channel until its
  active callback frame returns; this narrow retirement lease owns neither the
  fd nor its upper business object
- EventLoop retains a partial active batch and cursor across budgeted turns.
  Intervening owner-loop removal nulls the indexed observation before Channel
  ownership is released; no continuation queue owns Channel
- TimerQueue retains ready metadata not selected by the current timer budget,
  and the fixed control mailbox retains bits not selected by the current
  control budget

## 2.1 EventLoopControlSource
- A source handle is a copyable, non-owning scheduling capability
- A handle neither owns nor extends EventLoop lifetime
- Only the non-installed source-private control registry may create or revoke
  registrations; public callers cannot claim internal slots
- The registering subsystem owns the obligation to unregister on the owner
  thread before its callback target is destroyed
- EventLoop owns callback storage until unregister or EventLoop destruction
- Slot generation invalidation prevents copied stale handles from addressing a
  replacement registration

## 2.2 EventLoopLifecycleSource
- A lifecycle source is a copyable, non-owning, generation-tagged signal
  capability; it does not extend EventLoop or participant lifetime
- EventLoop owns an attached node from successful owner-thread attach until
  detach has invalidated its generation and all committed dirty/callback work
  for that generation has drained
- The node embeds its dirty-set link. Cross-thread signal may lock and mutate
  that link but allocates and owns no queue node
- Callback storage may retain the participant until detach reclamation; cycles
  from participant-owned handles back into the participant are forbidden
- A stale source owns nothing and cannot address reused node storage

## 2.3 Source-Private I/O Engine

- EventLoop uniquely owns one dual-interface adapter object. The byte-stable
  `unique_ptr<Poller>` member is only IOE-R1 storage compatibility; production
  EventLoop calls use the source-private Engine interface
- Engine readiness state borrows Channel. Accepted cancellation removes the
  exact registration; EventLoop invalidates the active-batch generation before
  the higher layer may release Channel ownership
- `commitCompletionSubmission` is called only after the kernel accepted an
  operation. A supplied lease then owns operation storage until terminal packet
  dequeue; synchronous submission rejection creates no Engine ownership
- `commitCompletionCancellation` adds a final-drain obligation but does not
  consume a packet or release its storage. Repeated cancellation commit is
  idempotent at the current IOCP backend
- EventLoop owns callback dispatch and containment. Engine owns no user callback
  and may not extend a callback target beyond its registration/operation lease
- The Engine remains Quiescing through EventLoop FinalDraining. Physical
  Shutdown and backend release occur during owner-thread EventLoop destruction
- On Linux, `EpollReadinessPort` owns the epoll descriptor, internal eventfd,
  fixed native/decoded batch storage, and registration identity maps. It borrows
  Channel targets and owns no callback or socket
- a `ReadinessNotice` is a value snapshot that borrows its target only after
  source-plus-generation validation. Removing the registration erases that
  generation before Channel release; a stale kernel token therefore owns and
  addresses nothing
- the public/platform-internal `EPollPoller` remains a compatibility shell over
  the same native port. EventLoop's production adapter uses the port directly
  and does not restore raw `epoll_event.data.ptr` delivery

## 3. Poller
- Poller does not own Channel
- Poller only maintains registration/mapping relationship
- Poller backend state must not outlive EventLoop
- The IOCP Poller owns its atomic wakeup-pending bit until owner-thread
  destruction. A queued wakeup packet owns no EventLoop, Channel, callback, or
  business payload; it only represents one-or-more logical scheduling requests
- Poller erases a registration only after exact fd-plus-pointer validation; it
  cannot erase a same-fd replacement on behalf of a stale Channel
- Native IOCP entries are decoded into fixed `CompletionNotice` value snapshots
  before scheduler publication. Each notice preserves operation address plus
  submission generation, kind, bytes, native error, terminal status, borrowed
  observer source/generation, and a source-private consumer
- terminal dequeue retires exactly that generation's kernel/final-drain state.
  An operation-owned terminal observer may update source-private transport
  bookkeeping on the owner thread, but owns no callback target and cannot
  invoke user code
- a retained operation lease owns only source-private operation/transport state;
  it does not grant application execution permission or keep TcpConnection
  ownership alive. The lease transfers to the typed wait batch at dequeue and
  survives observer dispatch/revocation. EventLoop releases it after the whole
  active batch is dispatched; direct native test consumers retire it explicitly
- the configured IOCP dequeue width borrows a prefix of the Poller's fixed
  64-entry storage; it allocates no packet array and owns no completion
- production Accept/Connect/Read/Write terminal results remain distinct typed
  notices and are never stored in Channel. Kernel-terminal bookkeeping and
  consumer-terminal bookkeeping are separate: dequeue clears native pending,
  while the direct consumer clears dispatch pending even when its Channel
  observer was revoked. A source-private slot cannot be reused between those
  two transitions
- every accepted observer-bound submission freezes the observer source and a
  process-unique Channel registration generation. EventLoop validates that
  source, Channel identity, and frozen generation before a callback and holds
  the Channel tie through it. A stale or same-address replacement observer
  still runs source-private consumer retirement but cannot reach TcpConnection
  or user code
- the installed Channel ABI temporarily retains its private single-operation
  mailbox and bounded Accept queue, but the production Engine path neither
  writes nor reads them; only the isolated legacy Poller shell uses the Accept
  queue until that shell and the reserved fields receive stable-surface review
- independent Accept operations for one listen Channel are appended through
  their operation-embedded links to one bounded callback queue in the current
  batch
- legacy Accept compatibility publication lends one bounded intrusive queue of
  exact identities; no queue node allocation or operation-storage transfer is
  introduced
- an IOCP retained lease owns operation storage but is not by itself a shutdown
  obligation. A successfully submitted operation canceled during teardown is
  marked exactly once as outstanding; packet dequeue clears the mark and
  releases any retained lease even when the Channel observer is null
- synchronous non-pending submission failure creates neither retained nor
  outstanding ownership
- fixed-storage accounting owns only atomic byte observations. It owns no
  Poller, completion entry, Channel, operation, or EventLoop and cannot extend
  any of their lifetimes

## 4. Channel
- Channel does not own fd by default
- Channel belongs logically to one EventLoop
- Channel may observe an upper-layer owner through tie/weak_ptr mechanism
- Channel callback dispatch must respect observed owner lifetime
- EventLoop active-batch slots observe Channel only while the matching
  registration generation remains valid
- successful remove revokes that observation before ownership may be released

## 5. TcpConnection
- TcpConnection owns its input/output buffer members
- each Buffer exclusively owns its vector storage. A retention trim transfers
  readable bytes into replacement storage before releasing the historical
  allocation; it owns no socket, EventLoop, protocol state, or callback
- TcpConnection owns its cumulative optional-notification drop counter
- TcpConnection does not own EventLoop
- TcpConnection lifecycle should be coordinated through shared ownership where needed
- Callback-triggered lifetime risks must be explicitly guarded
- An admitted deferred high-water/write-complete callback temporarily shares
  connection ownership; a rejected callback submission retains no delayed
  ownership and must not interrupt connection progress
- On Windows, the posting transport owns each `OVERLAPPED`, `WSABUF`, and
  referenced backing buffer until exactly one normal, error, or cancellation
  completion is consumed. A synchronous non-pending submission failure creates
  no completion ownership obligation
- For Windows writes, the Core-private IOCP transport owns a deque of stable
  string segments on behalf of TcpConnection. `WSABUF` borrows only the current
  segment suffix; the segment outlives the pending completion and is released
  only after its offset reaches the end or close has consumed every pending
  completion
- Cross-thread Accepted send owns one immutable string allocation in its
  executor closure, then transfers that allocation into the segment deque.
  Neither the legacy output Buffer nor a transport mirror owns a second full
  copy
- Each Windows TcpConnection IOCP transport uniquely owns at most one optional
  4 KiB read allocation. `WSABUF` borrows that allocation only while the read
  obligation is pending; no pool or cross-EventLoop return path exists, and
  final close releases it only after the completion is consumed
- TcpConnection owns its socket until explicit owner-loop close. After close,
  the IOCP transport and/or Poller retain operation storage until every
  completion obligation is consumed; object destruction is not the close
  trigger
- On Linux, TcpConnection revokes the Poller's non-owning Channel registration
  before closing and releasing the numeric fd; later connectDestroyed cleanup
  is idempotent and cannot erase a replacement Channel that reused that fd
- TcpConnection's lifecycle node may retain the connection close state through
  completion drain, but it detaches before final connection ownership is
  released

## 6. Timer / Scheduled Tasks
- Timer containers own timer metadata
- Scheduled callbacks do not imply ownership of arbitrary target objects
- Cancellation semantics must be explicit
- `TimerScheduleResult::Accepted` transfers timer metadata into current or
  already-accepted owner work. A rejected result carries no valid TimerId;
  EventLoop shutdown discards future timer metadata rather than extending loop
  lifetime until deadlines fire
- Repeating cadence mode and consecutive catch-up count are TimerQueue-owned
  metadata; neither creates ownership of the callback's captured targets
- DeadlineQueue owns only key/generation/deadline bucket metadata. Returned
  expirations are values and own no connection, session, callback, or EventLoop
- TcpServer owns its graceful-stop coordination state; returned shared futures
  observe the terminal result but do not own TcpServer
- TcpServer owns base stop-generation bookkeeping and one aggregate participant
  record per worker; each worker loop owns execution of its aggregate lifecycle
  node
- Base connection-map ownership is released before BaseReleased is published;
  the worker owns Channel/callback cleanup until its generation-tagged ack
- EventLoopThreadPool/thread objects remain owned until all worker acks have
  converged and join completes
- EventLoopThreadPool owns only per-worker selection metadata and numeric
  connection-load observations. TcpServer retains connection/map ownership,
  and each connection's EventLoop assignment remains fixed until release
- Acceptor owns its retry timer; stop/destruction cancels it before Acceptor
  storage is released
- on Windows, Acceptor owns one bounded fixed pool and every slot in it; each
  slot exclusively owns its accepted socket, `OVERLAPPED`, address buffer, and
  generation until accepted-fd transfer or terminal cleanup. Poller retains
  shared fixed-pool storage independently for each submitted operation through
  its terminal packet
- AcceptEx fixed-pool byte accounting follows that shared pool-state lifetime,
  not the shorter Acceptor pointer lifetime, and therefore remains nonzero
  while any Poller lease still retains the slot vector
- successful accepted-fd handoff removes the socket from its completed slot
  before user/server callback entry. A replacement operation may reuse that
  slot only after the old completion was published; it then owns a new socket
  and a new generation
- stop may revoke all slot Channel observers and release Acceptor ownership
  only after marking every submitted slot as a shutdown obligation. Retry
  retains Acceptor ownership and consumes every canceled slot callback before
  replenishing the same fixed-capacity pool
- Connector owns its Channel and connecting socket. Its cancellation self guard
  temporarily owns Connector for mandatory completion cleanup, while Poller
  independently owns the final-drain obligation and retained ConnectEx state;
  neither lease substitutes for the other

## 7. Accepted Socket Failure
- Acceptor owns an accepted fd until it transfers that fd through the new-
  connection callback
- TcpServer owns the transferred fd until TcpConnection construction succeeds
- Every rejected or failed accepted-socket setup path closes the fd exactly once
- TcpConnection performs all fallible construction work before its Socket
  claims an accepted fd. If construction fails, partial-object unwind owns no
  fd and the TcpServer Socket guard remains the sole closer; after the final
  non-throwing claim, TcpServer releases that guard before leaving the
  pre-armed rollback record's synchronization boundary
- Before TcpConnection construction for a worker, TcpServer owns one
  worker-lifecycle rollback record. The record is allocated and linked while
  the local Socket guard still owns the fd; failure to commit that record
  therefore closes only the raw fd and creates no TcpConnection
- each worker rollback registry is bounded by the selected EventLoop's normal
  functor capacity. Registry exhaustion rejects while the Socket guard still
  owns the fd; queue saturation cannot create an unbounded cleanup payload
- After construction transfers the fd, the rollback record temporarily shares
  the TcpConnection with provisional base bookkeeping and the normal
  establishment functor. Queue/setup rejection first removes the base map,
  selector load, peer/admission deadline, and functor owners, then the worker
  lifecycle callback performs connectDestroyed and releases the final record
  reference on the owner thread
- Successful normal-queue admission changes the rollback record to awaiting
  owner establishment while the base map and accepted functor retain the
  connection. Successful owner establishment disarms and reclaims the record
  on that owner. Owner-establishment failure closes there, retains the record's
  final reference across base map/load/admission rollback, and releases that
  reference only after the base acknowledgement returns to the owner
- Connector transfers a connected fd exactly once at new-connection callback
  entry. The receiver owns cleanup from that point and must establish RAII
  before fallible work; Connector does not retain a second fd owner
- IOCP association preservation is not ownership of a raw SOCKET value:
  TcpClient records it only in the no-user-code handoff interval and rolls it
  back if the replacement Channel cannot register, so callback failure cannot
  leave a stale association for a reused numeric handle
- TcpClient's connected-fd handoff is transactional. Any exception before the
  replacement Channel is established clears provisional `connection_` and
  request ownership, forgets the IOCP numeric association, and closes the fd
  before Connector settles the failed callback generation
- A copied TcpClientControl owns only shared mailbox storage and a non-owning
  EventLoop lifecycle capability. It never owns TcpClient, Connector, a
  TcpConnection, or EventLoop
- TcpClient destruction closes that mailbox and detaches its lifecycle node on
  the owner loop. Handles may outlive the client and observe
  OwnerUnavailable without dereferencing released client storage

## 8. Callback Exception State
- Exception records borrow no callback-owned storage; `std::exception_ptr`
  owns the captured exception for the duration of policy observation
- A callback-exception observer does not own or extend EventLoop lifetime
- TcpConnection remains owned by its existing shared lifecycle while callback
  failure converges through close/remove-before-destroy

## 9. TcpServer Admission State
- TcpServer owns admission counters, active-per-peer bookkeeping, bounded rate
  buckets, authentication deadline tokens, their DeadlineQueue, and its single
  driver/continuation TimerIds
- A rate bucket owns only a peer address value and finite attempt metadata; it
  never owns a connection or socket
- DeadlineQueue entries own only key/generation/deadline metadata. TcpServer's
  connection map remains the sole target lookup, and authentication, removal,
  stop, or destroy cancels the corresponding token/driver
- Rejected accepted sockets remain owned by TcpServer's local Socket guard and
  are closed exactly once before the callback returns
- Provisional accepted connections rejected after fd transfer are not released
  by the base callback. Their pre-armed worker rollback record owns exact-once
  close and final destruction; stop/join must wait until no armed record
  remains

## 9.1 TCP Output-Memory Budgets
- TcpConnection owns its per-connection pending-byte count and releases it
  together with every successfully reserved upper scope
- TcpServer owns one shared server budget and one shared budget identity per
  selected EventLoop; connections borrow those identities through shared
  ownership so accepted reservations can outlive base-map removal until the
  owner-loop write/close path releases them
- an optional process/global budget is caller-owned and may be shared by
  multiple TcpServer instances; no budget owns a server, EventLoop,
  TcpConnection, payload, callback, Buffer, or segment
- a reservation owns only an exact byte-accounting obligation. Later-scope
  rejection rolls back earlier scopes synchronously; accepted bytes release
  once after write completion or terminal discard

## 10. Cross-Layer Rule
- Lower reactor layers do not own higher business objects
- Higher layers may own reactor-layer wrappers, but their destruction path must respect thread/lifecycle rules

## 11. Metrics Ownership
- Callers share MetricsExporter ownership with recorder callbacks
- recorder callbacks own no EventLoop, connection, session, logic, broadcast,
  or transport object
- TaggedMetricsExporter owns immutable static label values and shares only its
  sink exporter
- MetricsSnapshot owns value copies and may outlive the exporter

## 11.1 Session, Pipeline, And Broadcast
- SessionManager exclusively constructs and mutates PlayerSession
- Shared const PlayerSession views are owner-loop observations only;
  SessionBinding, SessionSnapshot and BroadcastTarget are the cross-loop value
  surfaces
- A SessionBinding shares only revocable generation state. It owns no
  SessionManager, PlayerSession, endpoint, EventLoop or business object
- Pipeline connection state owns pending-auth frames and one immutable current
  binding; queued commands own value copies of that binding
- Broadcast plans temporarily share endpoint/payload ownership. Dispatcher
  reservations own accounting obligations in owner-task -> owner-byte ->
  global-byte order until queue rollback or endpoint-loop task completion, and
  release exactly once in reverse order
- BroadcastDispatcher state owns immutable per-owner budget identities for its
  lifetime. A budget identity owns only atomic counters and never owns an
  EventLoop, executor, endpoint, payload, task, or callback
- PacketFramer exclusively owns its ring storage. Retention trim copies only
  its current logical bytes, preserves ring order, and releases the historical
  allocation without acquiring transport/session ownership

## 12. Destruction Rule
- Destruction of lifecycle-sensitive objects must not violate owner-thread assumptions
- “remove before destroy” must be enforced where registration exists
- No object should remain registered in Poller after its effective destruction path begins

## 13. Forbidden
- Implicit transfer through raw pointer handoff with no documented owner
- Shared ownership used as a substitute for lifecycle design
- Poller owning Channel
- Channel owning EventLoop
- A stale Channel remove erasing another Channel that reused the same fd
- A control-source callback capturing an owner whose lifetime is not protected
  through explicit unregister or revocable observation
- Releasing a lifecycle callback target immediately after detach request while
  a committed notification/callback frame still owns that generation
- Letting TcpServer join a worker before its BaseReleased/worker-ack generation
  has converged

## 14. Fault and Endurance Evidence
- each fault cycle owns and closes every raw client socket it creates
- TcpServer retains ownership of accepted sockets and graceful-stop state
- the endurance supervisor owns the child-process handle, captured log,
  checkpoint, and final evidence; heartbeats copy values and own no runtime
  networking object
