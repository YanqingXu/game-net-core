# Epoll/IOCP Platform I/O Design

## Goal

Guarantee that the Reactor/TCP foundation uses high-performance native I/O
backends: Linux uses `epoll`, and Windows uses IOCP. The Windows path must not
promote WinSock `select()` as an accepted backend.

## Current State

- Linux already builds `EPollPoller` through `PollerFactory`.
- Windows previously selected `SelectPoller`, and `platform_runtime.intent.md`
  documented select as the Windows backend before this spec.
- The existing `Poller` API is readiness-oriented. IOCP is completion-oriented,
  so a real Windows implementation needs overlapped socket operations, not only
  a renamed poller class.
- A local Windows CI commit existed only locally and was not pushed. It is being
  replaced by this IOCP-focused migration direction.

## Recommended Approach

Use a staged IOCP migration:

1. Align intent and guardrails.
   - Update poller/platform intents to require Linux epoll and Windows IOCP.
   - Add static guards that fail if Windows default selection uses
     `SelectPoller`.
   - Keep Windows CI deferred until IOCP is the tested path.

2. Introduce Windows IOCP primitives.
   - Add IOCP backend files under `include/gamenet/core/net/poller` and
     `src/core/net/poller`.
   - Own the IOCP handle inside the Windows backend.
   - Register sockets with `CreateIoCompletionPort`.
   - Wait with `GetQueuedCompletionStatus` or `GetQueuedCompletionStatusEx`.

3. Move Windows TCP read/write to overlapped operations.
   - Use `WSARecv` and `WSASend` with `OVERLAPPED` state.
   - Translate completions back into the owning EventLoop thread.
   - Preserve callback ordering: connection callbacks and message callbacks still
     run on the owning EventLoop thread.

4. Re-enable Windows CI.
   - Configure, build, and test on `windows-2022`.
   - CI must validate the IOCP source selection and run the public unit,
     contract, and integration tests.

## Non-Goals

- Do not add HTTP, WebSocket, RPC, UDP, KCP, TLS, metrics, or game pipeline
  modules.
- Do not introduce a fake IOCP wrapper that internally keeps readiness/select
  semantics.
- Do not execute user callbacks from arbitrary IOCP worker threads.
- Do not make protocol layers aware of Windows completion details.

## Ownership And Threading

- Linux `EPollPoller` remains owned by `EventLoop`.
- Windows IOCP backend is owned by `EventLoop` or an explicitly documented
  loop-owned backend object.
- Socket ownership remains with `Socket`/`TcpConnection`.
- Completion state must not outlive the connection that owns the socket.
- Cross-thread user requests still marshal through `EventLoop::runInLoop` or
  `EventLoop::queueInLoop`.
- IOCP completions are translated into the same owner-loop callback context used
  by Linux.

## Tests And Guards

- Static guard: Windows default backend must mention IOCP and must not select
  `SelectPoller`.
- Static guard: Windows CMake source selection must include IOCP sources before
  Windows CI is enabled.
- Contract tests: existing EventLoop, Poller, TcpConnection, TcpServer, and
  TcpClient tests remain the public contract.
- CI: Linux Debug/ASan/Release remains green while IOCP is introduced; Windows
  CI is enabled only when IOCP code is present.

## Open Implementation Notes

- The existing readiness `Poller::poll()` API may remain as a compatibility
  surface if the IOCP backend drains completion packets and fills active
  channels, but Windows read/write readiness must come from posted overlapped
  operations.
- If the compatibility surface becomes too contorted, introduce a narrow
  platform completion adapter under the EventLoop without exposing it to upper
  layers.
- Select-based files are removed from the active Windows build once IOCP is
  introduced; they must not return as the promoted Windows backend.
