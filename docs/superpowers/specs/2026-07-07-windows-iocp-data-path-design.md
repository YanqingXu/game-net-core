# Windows IOCP Data Path Design

## Goal

Finish the Windows backend for the current Reactor / TCP foundation by moving
accept, connect, read, and write onto real IOCP overlapped operations while
preserving the existing owner-loop callback model.

This design is limited to the active core modules. It does not promote HTTP,
WebSocket, RPC, UDP, KCP, TLS, metrics, protocol framing, transport abstraction,
session, logic loop, broadcast, or game pipeline modules.

## Current Evidence

The repository now selects `IocpPoller` on Windows and the IOCP wakeup path is
covered by `contract.event_loop.test_event_loop` and
`contract.timer_queue.test_timer_queue`. A local VS2026 Debug run currently
passes 18/29 tests. The remaining 11 failures are concentrated in the Windows
network data path: accept, connect, poller readiness, TcpConnection lifecycle,
TcpServer/TcpClient lifecycle, and the TCP echo integration.

The root cause is architectural, not a timer bug: the Windows backend owns a
completion port and can post wakeup completions, but sockets are still used
through readiness-style `accept`, `connect`, `recv`, and `send` paths. IOCP
requires posted overlapped operations such as `AcceptEx`, `ConnectEx`,
`WSARecv`, and `WSASend`.

## Recommended Architecture

Use a loop-owned Windows IOCP operation layer under the existing Reactor/TCP
public contracts:

- `IocpPoller` owns only the completion port, socket association, completion
  wait, and translation from completion packets to active `Channel` events.
- Windows-only operation records own each `OVERLAPPED` object and completion
  metadata. These records are owned by the module that owns the operation:
  Acceptor for accepts, Connector for connects, and TcpConnection or a
  dedicated TcpConnection transport helper for reads and writes.
- `TcpConnection` remains the public lifecycle center. It still owns the socket,
  channel, buffers, public state machine, and user-visible callbacks.
- User callbacks continue to execute only on the owner `EventLoop` thread.

This keeps the current Linux epoll path stable while allowing Windows to use a
completion-oriented implementation internally.

## Rejected Approaches

### Put all overlapped state inside IocpPoller

This would make `IocpPoller` an owner of connection operations and would blur
Poller's current responsibility. The existing intent says Poller is not the
Channel owner, not the callback dispatcher, and not the connection lifecycle
owner.

### Put all IOCP logic directly inside TcpConnection

This is viable for the first read/write experiment but becomes too large once
accept, connect, cancellation, pending-write drain, and later transport variants
are included. `TcpConnection` should remain the public state machine and may
delegate transport-specific operation storage to a loop-owned helper.

### Reintroduce select or readiness polling on Windows

This would make tests easier to green temporarily, but it directly contradicts
the platform intent and Windows IOCP milestone. WinSock `select()` is not an
accepted promoted backend for this migration.

## Component Boundaries

### IocpPoller

Responsibilities:

- Create and close the IOCP handle.
- Associate sockets with the IOCP handle through `CreateIoCompletionPort`.
- Wait through `GetQueuedCompletionStatus`.
- Recognize explicit wakeup completions posted with
  `PostQueuedCompletionStatus`.
- For real I/O completions, recover the operation record from `OVERLAPPED`,
  store byte count and error metadata on that record, set the owning Channel's
  `revents`, and return the Channel in the active list.

Non-responsibilities:

- Does not own `Channel`.
- Does not own accepted or connected sockets.
- Does not own connection buffers.
- Does not decide TcpConnection close semantics.
- Does not run user callbacks.

### Windows Operation Records

A Windows-only operation record is an internal object with this shape:

```cpp
enum class IocpOperationKind {
    Accept,
    Connect,
    Read,
    Write,
};

struct IocpOperation {
    OVERLAPPED overlapped{};
    IocpOperationKind kind;
    Channel* channel{nullptr};
    DWORD bytesTransferred{0};
    DWORD error{0};
};
```

The actual implementation may wrap this record in small Acceptor, Connector,
and TcpConnection helper classes, but each operation must have stable storage
until the kernel completes it or cancellation/close ordering makes completion
safe to ignore.

### Acceptor

On Windows, Acceptor posts one or more `AcceptEx` operations after the listen
socket is associated with IOCP. Completion is translated into a read event for
the listen Channel. `Acceptor::handleRead()` consumes completed accepts from the
Windows helper and invokes the existing new-connection callback on the owner
loop. After a completed accepted socket is delivered or closed, the helper
posts the next accept while the Acceptor is still listening.

### Connector

On Windows, Connector resolves the `ConnectEx` extension function, binds the
socket as required by `ConnectEx`, posts an overlapped connect, and translates
completion into the existing write event path. Successful completion updates the
connect context before ownership transfers to TcpClient. Failed completion uses
the existing retry and timeout paths.

### TcpConnection

On Windows, TcpConnection uses a loop-owned helper for overlapped reads and
writes. The helper owns read/write operation records and temporary WSABUF state.
When a read completion is dispatched, `TcpConnection::handleRead()` consumes the
completed bytes already stored in the input buffer instead of calling `recv`
again. When a write completion is dispatched, `TcpConnection::handleWrite()`
retrieves completed bytes from the helper, drains output buffer state, queues
write-complete if the buffer is empty, and issues shutdown when pending output
has drained.

Linux continues to use the current Buffer `readFd` and `writeFd` readiness path.

## Data Flow

### Wakeup

Cross-thread queued work still calls `EventLoop::wakeup()`. On Windows the
Poller backend posts a wakeup completion packet. No Channel callback is needed
for this completion.

### Accept

1. `Acceptor::listen()` starts listening and enables reading on the listen
   Channel.
2. The Windows helper posts `AcceptEx` with stable operation storage.
3. `IocpPoller::poll()` receives the completion, stores metadata on the
   operation, marks the listen Channel readable, and returns it active.
4. `Acceptor::handleRead()` completes accept bookkeeping, delivers the accepted
   socket through the existing callback, and posts the next accept if listening.

### Connect

1. `Connector::connect()` creates an overlapped TCP socket and posts
   `ConnectEx`.
2. `IocpPoller::poll()` receives the completion, stores metadata on the
   operation, marks the connector Channel writable or errored, and returns it
   active.
3. `Connector::handleWrite()` completes the connect context, validates
   `SO_ERROR`, and transfers the socket on success. Failure uses existing retry
   behavior.

### Read

1. `TcpConnection::connectEstablished()` enables reading and posts an initial
   `WSARecv`.
2. `IocpPoller::poll()` receives a read completion and marks the connection
   Channel readable.
3. `TcpConnection::handleRead()` consumes the completed read bytes from the
   Windows helper, invokes the message callback when bytes are available, treats
   zero bytes as peer close, and posts the next `WSARecv` while connected.

### Write

1. `TcpConnection::sendInLoop()` writes immediately through `WSASend` when no
   send is already pending, or appends to the existing output buffer.
2. `IocpPoller::poll()` receives a write completion and marks the connection
   Channel writable.
3. `TcpConnection::handleWrite()` consumes completed bytes, posts the next
   `WSASend` if output remains, queues write-complete when output drains, and
   performs pending shutdown after the final completion.

## Error And Close Ordering

- Every completion carries an operation kind, byte count, and Win32/WinSock
  error code.
- Failed accept/connect/read/write completions must converge on the module's
  existing error path.
- Peer EOF is represented by a successful read completion with zero bytes.
- Repeated close/error/forceClose paths must stay single-shot.
- Socket close and Channel removal must happen on the owner loop thread.
- A pending completion that arrives after logical close must be recognized by
  operation state and must not dispatch duplicate user callbacks.

## Testing Strategy

Use the existing public contract tests as the promotion target. Add focused
static guards for IOCP data-path structure before adding runtime behavior, then
green Windows runtime contracts in this order:

1. Poller completion operation metadata guard.
2. Acceptor contract.
3. Connector contract.
4. TcpConnection read/peer-close lifecycle.
5. TcpConnection write-complete, high-water, shutdown-pending-output, and peer
   reset lifecycle.
6. TcpServer/TcpClient lifecycle.
7. TCP echo integration.

## Implementation status

The IOCP data path has been implemented for the current Reactor / TCP
foundation and is represented by the `windows-msvc` workflow job. Local VS2026
Debug verification has reached 37/37 configured CTest tests and the Windows
install/package consumer gate also passes through `find_package(GameNetCore)`
and `GameNet::core`. The remote green status is established by ci #17 on main.

## Compatibility

The public API should not change. Linux epoll behavior should remain source and
behavior compatible. Windows-specific helper headers may live under
`include/gamenet/core/net/platform/` only when they are internal building blocks
for the current core target and do not expose higher protocol concepts.

## Success Criteria

- No active Windows source selection uses `SelectPoller` or WinSock `select()`.
- Windows IOCP wakeup remains green.
- Windows accept/connect/read/write use posted overlapped operations.
- Existing lifecycle and callback ordering contracts pass on Windows.
- Full Windows CTest reaches 37/37 for the current test set before the Windows
  workflow gate is treated as green.
- Documentation states that Windows support is represented by the
  `windows-msvc` workflow job, with remote green status established by ci #17
  on main.
