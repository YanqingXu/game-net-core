# Repository Guidelines

## Project Identity

game-net-core is an intent-driven C++23 networking foundation for game servers.
It is the component-split migration target for the larger `mini_trantor`
project.
It is not only a codebase:

- intent is first-class
- code is derived artifact
- tests are contract enforcement
- rules document lifecycle and thread-affinity boundaries
- documentation records architectural understanding

## Intent-Driven Workflow

Never start from code first when a core module is important. Start from:

1. intent
2. invariants
3. threading rules
4. ownership rules
5. contracts
6. tests
7. implementation

Always read the relevant file in `intents/` before generating or modifying
core code. Then read the relevant files in `rules/`.

Core module changes must answer:

1. Which loop/thread owns this module?
2. Who owns it and who releases it?
3. Which callbacks may re-enter?
4. Which operations are allowed cross-thread, and how are they marshaled?
5. Which specific test file verifies this change?

## Scope

Keep the first migration focused on the Reactor/TCP foundation extracted from
`mini_trantor`:

- Logger, Timestamp, noncopyable
- InetAddress, Buffer, SocketTypes, SocketsOps, Socket
- Channel, Poller, EPollPoller, IocpPoller, PollerFactory
- Wakeup, TimerQueue, EventLoop
- Acceptor, Connector
- EventLoopThread, EventLoopThreadPool
- TcpConnection, TcpServer, TcpClient

Do not add HTTP, WebSocket, RPC, UDP, KCP, TLS, metrics, or game-server pipeline
modules until the core target has stable tests and examples.

Preserved intent files for deferred modules are design assets only. They do not
expand current implementation scope by themselves.

Overall migration stages:

1. Initialize the `game-net-core` project skeleton.
2. Migrate the Reactor / TCP core.
3. Split CMake targets and test structure.
4. Gradually migrate protocol, transport, game foundation, and experimental
   modules.

## Layout

- Public headers live under `include/gamenet/...`.
- Implementations live under `src/...`.
- Tests are split into `tests/unit`, `tests/contract`, and `tests/integration`.
- Examples are kept minimal and runnable.
- Intent files live under `intents/`.

## Build

- Use CMake 3.20 or newer.
- Use C++23 without compiler extensions.
- Keep targets small and explicit.

## Style

- Prefer `gamenet::net` for the first core migration.
- Add broader namespaces such as `gamenet::protocol`, `gamenet::transport`,
  `gamenet::game`, and `gamenet::experimental` only when those modules exist.
- Preserve thread-affinity, ownership, and testing rules in `rules/`.
- Prefer explicit state machines over scattered boolean flags.
- Prefer narrow responsibilities over large catch-all classes.
- Do not mix business logic into the Reactor core.

## Review Order

Review important core changes in this order:

1. intent correctness
2. public contract correctness
3. invariants
4. thread-affinity correctness
5. ownership correctness
6. lifecycle correctness
7. implementation details
8. test completeness
