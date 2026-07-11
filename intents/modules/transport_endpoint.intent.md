---
status: active
target: GameNet::transport
migration_source: mini_trantor
promote_gate: none
artifact_kind: installed-library
migration_mode: redesign
source_commit: 3eba368475a68f677aae920d4f299b155db23d57
source_paths: mini/net/transport/TransportEndpoint.h
---

# Module Intent: TransportEndpoint

## Intent

`GameNet::transport` provides the narrow boundary through which session and
game layers identify, send through, and close a network endpoint without
depending on `TcpConnection`.

## Contract

- An endpoint exposes only a stable `TransportSessionId`, `ownerExecutor()`,
  `send(bytes)`, `close(reason)`, and `isOpen()`.
- It never parses protocol bytes or owns player/business state.
- `TcpTransportEndpoint` adapts one `TcpConnection` through a weak reference;
  it does not extend the connection lifetime or change connection callbacks.
- `TcpTransportEndpoint` construction is owner-loop-only. The caller must hold a
  strong `TcpConnection` reference and the connection's `EventLoop` must dominate
  the complete constructor call. Construction after owner admission has closed
  is rejected; the endpoint does not attempt to recover an executor from a
  connection whose loop lifetime is already ending.
- `send` and `close` are owner-loop-only and return `WrongThread` rather than
  hiding a second scheduling policy. Callers marshal through the copyable
  `ownerExecutor()` handle and handle rejected admission explicitly.
- A task successfully admitted before loop shutdown may still call `send` or
  `close` during EventLoop's final accepted-work drain. New tasks are rejected
  once admission closes, and owner operations after that drain report
  `OwnerUnavailable`.
- `ownerExecutor()` does not own EventLoop and remains safely queryable after
  loop teardown; its stable identity is used only for owner grouping.
- Result precedence is deterministic. An expired weak connection reports
  `Closed` even when its former owner is also unavailable. While the connection
  object is still alive, closed owner admission/final-drain completion reports
  `OwnerUnavailable` before the connection state snapshot is considered;
  otherwise a live owner called from the wrong thread reports `WrongThread`, and
  an owner-thread operation on a disconnected connection reports `Closed`.
- `isOpen()` is a cross-thread-safe state snapshot. It performs no loop-owned
  mutation and does not imply a future send will be accepted.
- Normal/going-away close requests use graceful shutdown. Protocol, replacement,
  idle-timeout, and overload reasons force closure.

## Ownership And Re-entry

- The layer above owns endpoint objects. `TcpServer`/`TcpConnection` retain
  their existing ownership and release paths.
- `TcpTransportEndpoint` locks its weak connection before any connection access
  and uses the EventLoop executor state rather than dereferencing a cached raw
  owner-loop pointer.
- No endpoint callback exists, so endpoint operations add no re-entry path.
- Connection callbacks may still re-enter upper layers under the existing core
  contract; the adapter does not install or replace them.

## Verification

- `tests/contract/transport/test_tcp_transport_endpoint_contract.cpp` verifies
  identity, weak ownership, wrong-thread rejection, owner-loop send/close, and
  cross-thread open-state observation.
- `tests/contract/transport/test_tcp_transport_endpoint_lifetime.cpp` verifies
  endpoint-outlives-connection/loop, rejected scheduling after teardown, and
  completion of endpoint send/close work accepted before the final drain. It
  also keeps the connection object alive after final-drain completion and
  verifies that new scheduling is rejected and direct send/close report
  `OwnerUnavailable`.

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: narrow endpoint identity/send/close boundary, endpoint owner
  context, and no player/business ownership.
- Deferred from the source design: UDP adaptation and broader transport-manager
  policy remain outside this active TCP adapter.
- Dropped behavior: none; raw owner-loop scheduling is replaced by the
  EventLoop-owned executor admission contract during Phase 4 hardening.
