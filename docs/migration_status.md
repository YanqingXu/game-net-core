# Migration Status

Last checked: 2026-07-06

## Current Task Goal

`game-net-core` is the component-split migration target for the larger
`mini_trantor` project.

The active execution target is still the Reactor / TCP foundation. Higher-level
protocol, transport, game foundation, and experimental modules remain deferred
until the current core has stable targets, tests, and examples.

## Phase Status

| Phase | Scope | Current status |
| --- | --- | --- |
| 1 | Initialize the `game-net-core` project skeleton | Present: top-level CMake, README, AGENTS, docs, intents, rules, include/src/tests/examples layout |
| 2 | Migrate Reactor / TCP core | Present: base utilities, socket helpers, Channel/Poller/EventLoop/TimerQueue, Acceptor/Connector, TcpConnection/TcpServer/TcpClient |
| 3 | Split CMake targets and test structure | Present: `gamenet_core`, echo examples, unit/contract/integration test directories, and Acceptor/Buffer/Channel/Connector/InetAddress/Poller/Socket/TcpClient/TcpServer/EventLoopThread/EventLoopThreadPool contract tests |
| 4 | Gradually migrate protocol / transport / game foundation / experimental | Deferred: intent files are preserved as design assets only |

## Verification State

Static documentation checks passed for the migration-goal wording. Phase 3 now
also registers focused Acceptor, Buffer, Channel, Connector, InetAddress,
Poller, Socket, TcpClient, TcpServer, EventLoopThread, and EventLoopThreadPool
contract tests. Build and CTest verification could not be run in this
environment because no local CMake or C++ compiler was found in PATH or common
Windows installation locations.

Before promoting Phase 4 work, run the local equivalent of the CI gate and keep
the Reactor / TCP tests green.
