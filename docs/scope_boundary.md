# Scope Boundary

game-net-core starts as the Reactor/TCP extraction target from `mini_trantor`.
It should stay a compact networking foundation for game servers while the larger
project is split by component boundaries.

## In Scope First

- Reactor-style EventLoop
- Channel and Poller abstraction
- TimerQueue and wakeup mechanics
- TCP accept/connect/connection lifecycle
- Buffer and address utilities
- Minimal echo-server example
- Unit, contract, and integration tests for the core path

## Deferred

- Protocol framing
- Transport abstraction
- HTTP
- WebSocket
- RPC
- UDP
- KCP
- PMTU / FEC
- TLS
- GameServerPipeline
- SessionManager
- LogicLoop
- Broadcast
- MetricsExporter

Deferred modules should be introduced only after the core target builds and has
focused tests.
