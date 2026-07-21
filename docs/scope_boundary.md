# Scope Boundary

game-net-core starts as the Reactor/TCP extraction target from `mini_trantor`.
It should stay a compact networking foundation for game servers while the larger
project is split by component boundaries.

## Active Scope

- Reactor-style EventLoop
- Channel and Poller abstraction
- TimerQueue and wakeup mechanics
- TCP accept/connect/connection lifecycle
- Optional bounded global/per-peer connection admission, per-peer fixed-window
  attempt limiting, and transport-level unauthenticated deadlines
- Bounded per-connection input/output admission and owner-loop high/low-water
  read backpressure
- Buffer and address utilities
- Minimal echo-server example
- Unit, contract, and integration tests for the core path
- Length-delimited PacketFramer under `GameNet::protocol`
- TransportEndpoint and the TCP adapter under `GameNet::transport`
- Network-only PlayerSession and SessionManager lifecycle state
- Value-type GameCommand, bounded queue, and fixed-tick LogicLoop
- Owner-loop BroadcastRouter/Dispatcher with explicit fanout/byte budgets
- Thread-safe MetricsExporter aggregation and non-blocking Core/Logic/Broadcast
  producer adapters
- A non-installed GameServerPipeline composition example and integration test

## Deferred

- Rich game packet headers and object codecs
- HTTP
- WebSocket
- RPC
- UDP
- KCP
- PMTU / FEC
- TLS
- A production/all-in-one GameServerPipeline library

Deferred modules require their own promoted intent, target, contracts, and
focused tests. Existing Phase 4 foundations do not authorize them implicitly.

Do not keep empty source or public-header placeholders for deferred modules.
Their directories should appear only when the module is intentionally promoted
with matching intent, tests, CMake targets, and migration-status updates.

## Dependency Direction

- `GameNet::core` cannot depend on any Phase 4 target.
- protocol and transport remain independent foundations above core.
- game_session may depend on transport; game_logic and broadcast may depend on
  session/transport value contracts.
- the pipeline example composes layers but is neither installed nor linked by
  core.
- UDP/KCP may implement the transport interface later, but only as experimental
  targets.
