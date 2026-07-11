---
status: active
target: GameNet::game_session
migration_source: native
promote_gate: none
---

# Module Intent: PlayerSession And SessionManager

## Intent

The session layer binds authenticated player identity to a current transport
endpoint while owning only network-session lifecycle state. It must not own
inventory, combat, map, room, account-provider, or other business data.

## Ownership And Threading

- One explicit management `EventLoop` owns `SessionManager`, every
  `PlayerSession`, and both player/transport indexes.
- Direct lookup and mutation APIs are management-loop-only.
- I/O loops use `postAuthenticate`, `postOffline`, and `postHeartbeat`; these
  enqueue work and never wait synchronously for the management loop.
- Authentication completion callbacks run on the management loop after indexes
  are consistent. They may re-enter management-loop-only APIs; no lock is held.
- Endpoint close requests are marshaled to each endpoint's owner loop.

## Lifecycle Invariants

- A player has at most one current session and a transport id indexes at most
  one session.
- Rebind replaces the transport index atomically on the management loop.
- A delayed offline event for a replaced transport cannot remove the rebound
  session.
- Duplicate login behavior is explicit: replace the old endpoint or reject the
  new endpoint.
- Heartbeat/activity timestamps and idle expiry are session lifecycle state,
  not player business state.

## Verification

`tests/contract/game_session/test_session_manager_contract.cpp` verifies
creation, duplicate login policies, rebind, stale disconnect cleanup,
heartbeat, idle expiry, endpoint-loop closure, and cross-thread async submit.
