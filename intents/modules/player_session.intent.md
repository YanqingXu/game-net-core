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
  enqueue work and never wait synchronously for the management loop. Every
  post returns `DispatchResult`; an Accepted post invokes its optional terminal
  callback exactly once with the management-side result, including shutdown
  that linearizes ahead of the queued task.
- Cross-thread posts use the management loop executor and weakly observe a
  shared `LifetimeState`. That state has an atomic `alive` permission bit which
  shutdown/destruction revokes before facade teardown. Locking or otherwise
  strongly retaining the state only proves the state object still exists; it
  never extends permission to dereference `SessionManager`. Every queued task
  rechecks `alive` immediately before facade access, so work that reaches the
  loop after revocation skips facade access and invokes only its optional
  terminal callback with the revocation result.
- Authentication completion callbacks run on the management loop after indexes
  are consistent. They may re-enter management-loop-only APIs; no lock is held.
- Each successful authentication/rebind publishes a monotonically increasing
  `SessionBinding(sessionId, transportId, generation)`. Binding tokens are
  immutable values backed by revocable atomic state, may cross loops, and can
  only answer whether that exact generation is still current. They expose no
  mutable session or endpoint state.
- `SessionManager` keeps mutable session ownership private. Authentication and
  lookup APIs expose `shared_ptr<const PlayerSession>`, so callers cannot rebind
  or change lifecycle state behind the manager's player/transport indexes.
  These views remain management-loop-only: constness is not a cross-thread
  snapshot guarantee, and callers must copy the value data they need before
  handing work to another loop.
- `PlayerSession` construction and every mutation are private to
  `SessionManager`. Broadcast and cross-loop consumers receive immutable
  `SessionSnapshot`/`BroadcastTarget` values rather than aliases to manager-
  owned session objects.
- Endpoint close requests are marshaled to each endpoint's owner loop.
- SessionManager shutdown/destruction occurs on its management loop while that
  loop is alive. Closing executor admission does not relax this owner-thread
  requirement. Destruction revokes the lifetime token before member teardown.
- `shutdown()` is a one-shot management-loop transition. It first revokes the
  cross-thread lifetime token, atomically removes both indexes, marks every
  indexed session Offline, and requests `GoingAway` close on each endpoint's
  owner loop. Repeated shutdown is a no-op. Accepted work queued before
  revocation invokes its optional terminal callback with `Shutdown`; posts
  submitted after revocation return `Shutdown` immediately and are not
  admitted.
- After shutdown, direct `authenticate`, `offline`, `heartbeat`, `expireIdle`,
  and `expireIdleBatch` calls cannot mutate session state: they return
  `Rejected`, `false`, `false`, zero, and an empty batch respectively.
  Cross-thread posts return `Shutdown`; already-Accepted posts receive a
  `Shutdown` terminal callback without dereferencing the revoked manager.
  Lookups remain available on the management loop and observe the already-empty
  indexes. A rejected caller-supplied endpoint that was never indexed remains
  caller-owned.

## Lifecycle Invariants

- A player has at most one current session and a transport id indexes at most
  one session.
- Authentication with a transport id already bound to a different endpoint is
  rejected as a protocol identity collision, including a second endpoint that
  claims the same player identity. Neither index changes; the distinct newcomer
  endpoint is closed with `ProtocolError`. Re-authentication is `Existing` only
  for the same player and the same endpoint object.
- Rebind replaces the transport index atomically on the management loop.
- Rebind revokes the previous binding generation before publishing the new
  endpoint/index binding. Offline, expiry, shutdown, and replacement revoke the
  old generation before any endpoint close is requested.
- A delayed offline event for a replaced transport cannot remove the rebound
  session.
- Duplicate login behavior is explicit: replace the old endpoint or reject the
  new endpoint.
- Heartbeat/activity timestamps and idle expiry are session lifecycle state,
  not player business state.
- Each active session owns one manager-controlled, generation-tagged idle
  deadline. Authentication, rebind, and heartbeat replace it; offline,
  replacement, expiry, and shutdown cancel it.
- Idle expiration advances only due non-empty deadline buckets and processes at
  most the configured per-advance budget. A ready remainder is explicit so the
  pipeline can continue bounded batches without rescanning the player index or
  waiting a full sweep interval.
- A Logic command or output carrying a stale binding generation cannot produce
  a handler side effect or endpoint send even when it was admitted before a
  duplicate-login replacement.

## Verification

- `tests/contract/game_session/test_session_manager_contract.cpp` verifies
  creation, duplicate login policies, transport-id uniqueness, rebind, stale
  disconnect cleanup, heartbeat, idle expiry, read-only public session views,
  private construction/mutation, immutable snapshots, generation rollover and
  cross-thread typed async submit, generation-safe deadline replacement, and
  bounded same-bucket idle-expiry continuation.
- `tests/contract/game_session/test_session_manager_dispatch.cpp` saturates the
  management queue and verifies QueueFull, Shutdown, OwnerUnavailable and
  exactly-once terminal callback behavior for authenticate/offline/heartbeat.
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
- `gamenet_phase4_benchmark --scenario session-expiry-scan|session-expiry`
  records exact due candidates, expired/remaining/close counts, bounded-batch
  advances, and normalized no-ready-work versus expiration/cleanup time at
  configurable session scales; it is opt-in and not a correctness CTest.

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: management-loop ownership, player/transport indexes,
  duplicate-login policy, stale-disconnect protection, and idle expiry.
- Deferred from the source design: account/auth-provider and business session
  data remain outside this networking foundation.
- Dropped behavior: none; the source lifetime-token protection is restored and
  transport-id uniqueness is strengthened by the Phase 4 contract.
