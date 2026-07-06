# Repository Guidelines

## Scope

Keep the first migration focused on the Reactor/TCP foundation:

- Logger, Timestamp, noncopyable
- InetAddress, Buffer, SocketTypes, SocketsOps, Socket
- Channel, Poller, EPollPoller, SelectPoller, PollerFactory
- Wakeup, TimerQueue, EventLoop
- Acceptor, Connector
- EventLoopThread, EventLoopThreadPool
- TcpConnection, TcpServer, TcpClient

Do not add HTTP, WebSocket, RPC, UDP, KCP, TLS, metrics, or game-server pipeline
modules until the core target has stable tests and examples.

## Layout

- Public headers live under `include/gamenet/...`.
- Implementations live under `src/...`.
- Tests are split into `tests/unit`, `tests/contract`, and `tests/integration`.
- Examples are kept minimal and runnable.

## Build

- Use CMake 3.20 or newer.
- Use C++23 without compiler extensions.
- Keep targets small and explicit.

## Style

- Prefer `gamenet::net` for the first core migration.
- Add broader namespaces such as `gamenet::protocol`, `gamenet::transport`,
  `gamenet::game`, and `gamenet::experimental` only when those modules exist.
- Preserve thread-affinity, ownership, and testing rules in `rules/`.
