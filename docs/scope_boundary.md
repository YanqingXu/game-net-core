# Scope Boundary

game-net-core starts as a compact networking foundation for game servers.

## In Scope First

- Reactor-style EventLoop
- Channel and Poller abstraction
- TimerQueue and wakeup mechanics
- TCP accept/connect/connection lifecycle
- Buffer and address utilities
- Minimal echo-server example
- Unit, contract, and integration tests for the core path

## Deferred

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
