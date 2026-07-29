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
- EventLoop owns the fixed control-source slot table, pending bitset, and
  registered callback storage
- EventLoop owns its dynamic lifecycle hub, attached-node registry, intrusive
  dirty links, generation counters, and callback storage
- EventLoop does not own business-layer connection semantics
- EventLoop may temporarily own one already-removed current Channel until its
  active callback frame returns; this narrow retirement lease owns neither the
  fd nor its upper business object

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

## 3. Poller
- Poller does not own Channel
- Poller only maintains registration/mapping relationship
- Poller backend state must not outlive EventLoop
- Poller erases a registration only after exact fd-plus-pointer validation; it
  cannot erase a same-fd replacement on behalf of a stale Channel
- IOCP batch entries are value snapshots and do not acquire Channel ownership.
  Publishing one entry releases exactly that operation's outstanding/retained
  backend lease before its Channel callback runs; a same-Channel entry deferred
  to the next round keeps both leases intact. Its transferred-byte and terminal
  error result is captured at dequeue time so later socket closure cannot
  mutate the observation
- an IOCP retained lease owns operation storage but is not by itself a shutdown
  obligation. A successfully submitted operation canceled during teardown is
  marked exactly once as outstanding; packet dequeue clears the mark and
  releases any retained lease even when the Channel observer is null
- synchronous non-pending submission failure creates neither retained nor
  outstanding ownership

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
- TcpServer owns its graceful-stop coordination state; returned shared futures
  observe the terminal result but do not own TcpServer
- TcpServer owns base stop-generation bookkeeping and one aggregate participant
  record per worker; each worker loop owns execution of its aggregate lifecycle
  node
- Base connection-map ownership is released before BaseReleased is published;
  the worker owns Channel/callback cleanup until its generation-tagged ack
- EventLoopThreadPool/thread objects remain owned until all worker acks have
  converged and join completes
- Acceptor owns its retry timer; stop/destruction cancels it before Acceptor
  storage is released
- on Windows, Acceptor owns the listen/accepted sockets and Channel observer,
  while Poller retains only the submitted AcceptEx operation state through its
  terminal packet; Acceptor may clear the observer and release its own storage
  after registering the shutdown obligation
- Connector owns its Channel and connecting socket. Its cancellation self guard
  temporarily owns Connector for mandatory completion cleanup, while Poller
  independently owns the final-drain obligation and retained ConnectEx state;
  neither lease substitutes for the other

## 7. Accepted Socket Failure
- Acceptor owns an accepted fd until it transfers that fd through the new-
  connection callback
- TcpServer owns the transferred fd until TcpConnection construction succeeds
- Every rejected or failed accepted-socket setup path closes the fd exactly once
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
  buckets, expiry records, and unauthenticated TimerIds
- A rate bucket owns only a peer address value and finite attempt metadata; it
  never owns a connection or socket
- Authentication timers borrow TcpServer through the existing revocable
  lifetime token and are canceled on authentication, removal, stop, or destroy
- Rejected accepted sockets remain owned by TcpServer's local Socket guard and
  are closed exactly once before the callback returns

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
  reservations own accounting obligations until queue rollback or endpoint-loop
  task completion, and release exactly once

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
