---
status: active
target: GameNet::game_session
migration_source: mini_trantor
promote_gate: none
artifact_kind: installed-library
migration_mode: redesign
source_commit: 3eba368475a68f677aae920d4f299b155db23d57
source_paths: mini/game/PlayerSession.h;mini/game/PlayerSession.cc;mini/game/SessionManager.h;mini/game/SessionManager.cc
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
- Cross-thread posts use the management loop executor and weakly observe a
  shared `LifetimeState`. That state has an atomic `alive` permission bit which
  shutdown/destruction revokes before facade teardown. Locking or otherwise
  strongly retaining the state only proves the state object still exists; it
  never extends permission to dereference `SessionManager`. Every queued task
  rechecks `alive` immediately before facade access, so work that reaches the
  loop after revocation becomes a no-op.
- Authentication completion callbacks run on the management loop after indexes
  are consistent. They may re-enter management-loop-only APIs; no lock is held.
- `SessionManager` keeps mutable session ownership private. Authentication and
  lookup APIs expose `shared_ptr<const PlayerSession>`, so callers cannot rebind
  or change lifecycle state behind the manager's player/transport indexes.
  These views remain management-loop-only: constness is not a cross-thread
  snapshot guarantee, and callers must copy the value data they need before
  handing work to another loop.
- Endpoint close requests are marshaled to each endpoint's owner loop.
- SessionManager shutdown/destruction occurs on its management loop while that
  loop is alive. Closing executor admission does not relax this owner-thread
  requirement. Destruction revokes the lifetime token before member teardown.
- `shutdown()` is a one-shot management-loop transition. It first revokes the
  cross-thread lifetime token, atomically removes both indexes, marks every
  indexed session Offline, and requests `GoingAway` close on each endpoint's
  owner loop. Repeated shutdown is a no-op. Work queued before revocation and
  posts submitted after revocation do not run and do not invoke authentication
  callbacks.
- After shutdown, direct `authenticate`, `offline`, `heartbeat`, and `expireIdle`
  calls cannot mutate session state: they return `Rejected`, `false`, `false`,
  and zero respectively. Lookups remain available on the management loop and
  observe the already-empty indexes. A rejected caller-supplied endpoint that
  was never indexed remains caller-owned.

## Lifecycle Invariants

- A player has at most one current session and a transport id indexes at most
  one session.
- Authentication with a transport id already bound to a different endpoint is
  rejected as a protocol identity collision, including a second endpoint that
  claims the same player identity. Neither index changes; the distinct newcomer
  endpoint is closed with `ProtocolError`. Re-authentication is `Existing` only
  for the same player and the same endpoint object.
- Rebind replaces the transport index atomically on the management loop.
- A delayed offline event for a replaced transport cannot remove the rebound
  session.
- Duplicate login behavior is explicit: replace the old endpoint or reject the
  new endpoint.
- Heartbeat/activity timestamps and idle expiry are session lifecycle state,
  not player business state.

## Verification

- `tests/contract/game_session/test_session_manager_contract.cpp` verifies
  creation, duplicate login policies, transport-id uniqueness, rebind, stale
  disconnect cleanup, heartbeat, idle expiry, read-only public session views,
  and cross-thread async submit.
- `tests/contract/game_session/test_session_manager_lifecycle.cpp` verifies
  queued authenticate/offline/heartbeat after destruction, forces live
  heartbeat/offline producers to remain active until a posted heartbeat has
  observably advanced `lastActivity` and the management loop then drains an
  overlapping offline post. It verifies no resurrection or index divergence,
  callback re-entry through lookup/heartbeat/rebind/offline cleanup, shutdown
  revocation before and after queue admission, repeated shutdown, blocked direct
  mutation after shutdown, and real management-to-distinct-endpoint-loop close
  thread/reason/single-shot behavior. It also queues two authentications plus
  heartbeat/offline work, destroys the manager from the first authentication
  callback, and proves the remaining work cannot use the still-strongly-held
  but revoked lifetime state to access the destroyed facade.

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: management-loop ownership, player/transport indexes,
  duplicate-login policy, stale-disconnect protection, and idle expiry.
- Deferred from the source design: account/auth-provider and business session
  data remain outside this networking foundation.
- Dropped behavior: none; the source lifetime-token protection is restored and
  transport-id uniqueness is strengthened by the Phase 4 contract.
