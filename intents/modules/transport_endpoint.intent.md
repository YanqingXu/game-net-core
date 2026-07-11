---
status: active
target: GameNet::transport
migration_source: native
promote_gate: none
---

# Module Intent: TransportEndpoint

## Intent

`GameNet::transport` provides the narrow boundary through which session and
game layers identify, send through, and close a network endpoint without
depending on `TcpConnection`.

## Contract

- An endpoint exposes only a stable `TransportSessionId`, `ownerLoop()`,
  `send(bytes)`, `close(reason)`, and `isOpen()`.
- It never parses protocol bytes or owns player/business state.
- `TcpTransportEndpoint` adapts one `TcpConnection` through a weak reference;
  it does not extend the connection lifetime or change connection callbacks.
- `send` and `close` are owner-loop-only and return `WrongThread` rather than
  hiding a second scheduling policy. Callers marshal through `ownerLoop()`.
- Normal/going-away close requests use graceful shutdown. Protocol, replacement,
  idle-timeout, and overload reasons force closure.

## Ownership And Re-entry

- The layer above owns endpoint objects. `TcpServer`/`TcpConnection` retain
  their existing ownership and release paths.
- No endpoint callback exists, so endpoint operations add no re-entry path.
- Connection callbacks may still re-enter upper layers under the existing core
  contract; the adapter does not install or replace them.

## Verification

`tests/contract/transport/test_tcp_transport_endpoint_contract.cpp` verifies
identity, weak ownership, wrong-thread rejection, owner-loop send, and close.
