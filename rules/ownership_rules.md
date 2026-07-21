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
- EventLoop does not own business-layer connection semantics

## 3. Poller
- Poller does not own Channel
- Poller only maintains registration/mapping relationship
- Poller backend state must not outlive EventLoop

## 4. Channel
- Channel does not own fd by default
- Channel belongs logically to one EventLoop
- Channel may observe an upper-layer owner through tie/weak_ptr mechanism
- Channel callback dispatch must respect observed owner lifetime

## 5. TcpConnection
- TcpConnection owns its input/output buffer members
- TcpConnection does not own EventLoop
- TcpConnection lifecycle should be coordinated through shared ownership where needed
- Callback-triggered lifetime risks must be explicitly guarded

## 6. Timer / Scheduled Tasks
- Timer containers own timer metadata
- Scheduled callbacks do not imply ownership of arbitrary target objects
- Cancellation semantics must be explicit
- TcpServer owns its graceful-stop coordination state; returned shared futures
  observe the terminal result but do not own TcpServer
- Acceptor owns its retry timer; stop/destruction cancels it before Acceptor
  storage is released

## 7. Accepted Socket Failure
- Acceptor owns an accepted fd until it transfers that fd through the new-
  connection callback
- TcpServer owns the transferred fd until TcpConnection construction succeeds
- Every rejected or failed accepted-socket setup path closes the fd exactly once

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

## 12. Destruction Rule
- Destruction of lifecycle-sensitive objects must not violate owner-thread assumptions
- “remove before destroy” must be enforced where registration exists
- No object should remain registered in Poller after its effective destruction path begins

## 13. Forbidden
- Implicit transfer through raw pointer handoff with no documented owner
- Shared ownership used as a substitute for lifecycle design
- Poller owning Channel
- Channel owning EventLoop

## 14. Fault and Endurance Evidence
- each fault cycle owns and closes every raw client socket it creates
- TcpServer retains ownership of accepted sockets and graceful-stop state
- the endurance supervisor owns the child-process handle, captured log,
  checkpoint, and final evidence; heartbeats copy values and own no runtime
  networking object
