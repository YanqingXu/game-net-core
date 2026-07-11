---
status: active
target: gamenet_game_server_pipeline_demo
migration_source: mini_trantor
promote_gate: none
artifact_kind: example
migration_mode: redesign
source_commit: 3eba368475a68f677aae920d4f299b155db23d57
source_paths: mini/game/GameServerPipeline.h;mini/game/GameServerPipeline.cc
---

# Use Case Intent: Game Server Pipeline Demo

## Intent

The first Phase 4 integration proves the complete composition without creating
an installed all-in-one framework class or allowing upper layers into core.

## Flow

`TCP bytes -> PacketFramer -> AUTH session admission -> GameCommandQueue ->
LogicLoop handler -> framed response -> TransportEndpoint`.

The demo protocol uses `AUTH <player-id>` for the first payload, replies with
`AUTH_OK`, and maps later payloads to `RESP <payload>`. These strings are example
semantics, not protocol-library responsibilities.

## Threading And Ownership

- Each TCP connection owner loop owns its `PacketFramer` through loop-owned
  connection context. It moves completed payload values to one management loop;
  no framer or input buffer crosses threads.
- The management loop owns authentication state, pending-auth frames, sessions,
  and the connection/session index. Disconnect removes that state before any
  later authentication callback can revive it.
- `LogicLoop` may use the management loop for the minimal configuration or a
  distinct caller-owned logic loop. Startup/stop coordination executes on that
  owner; command submit is thread-safe and outputs marshal back to management.
  A distinct borrowed logic loop must already be running before Pipeline
  construction and must remain alive and accepting until Pipeline `stop()` and
  destruction have completed on the management loop. The caller stops or
  destroys that EventLoop only after releasing the Pipeline.
- Each connection has explicit `Unauthenticated -> Authenticating -> Online ->
  Closing` state. The first valid AUTH moves to Authenticating before work is
  posted; a second AUTH in that state is a protocol error.
- Non-AUTH frames received while Authenticating are retained in arrival order
  under configured frame/byte hard limits. Success drains them after AUTH_OK;
  rejection, disconnect, overflow, or stop discards them.
- A delayed-auth TimerId is owned by the management-side connection state.
  Disconnect, close, overflow, and stop cancel it before releasing that state;
  the callback captures only revocable Pipeline state plus value identifiers,
  not an endpoint whose lifetime would be extended until the delay expires.
- The example option selects SessionManager's duplicate-login policy so both
  successful replacement and deterministic authentication rejection remain
  testable without introducing an account/authentication service.
- Session callbacks and management-side logic output callbacks may re-enter the
  composition; no queue lock is held.
- Output is always queued to the endpoint owner loop before `send`.
- The optional example-only stage observer runs on the observed stage thread;
  callers must synchronize shared observer state and must not block another loop.
- Worker callbacks use independent executor/state values and only dereference
  Pipeline on management after checking a shared callback state whose atomic
  `alive` bit is independent of the state object's lifetime. Holding a strong
  callback-state reference never extends permission to execute after revocation;
  every cross-loop callback rechecks `alive` immediately before Pipeline access,
  observer invocation, transport send, or transport close.
- `stop()` is one-shot and may be called synchronously by the Logic-stage
  observer when management and logic share one loop. It first atomically revokes
  cross-loop admission, stops LogicLoop on its owner, clears management state,
  and initiates server connection close. If stop re-enters from the current
  LogicLoop tick, ownership of the stopped LogicLoop is retired on its owner and
  destruction is deferred until that tick has returned. Distinct-loop stop still
  synchronously performs stop/destruction on the logic owner. If a distinct-loop
  logic callback is active, management-side `stop()` waits until that callback
  returns; callback revocation prevents its remaining output side effects.
- Endpoint sends require the callback state to remain live after any observer
  returns, so queued or observer-blocked output is canceled once stop revokes it.

## Scope

- `GameServerPipeline` remains in `examples/game_server_pipeline_demo` and is
  neither installed nor exported.
- The demo does not define account auth, message headers/codecs, player business
  state, HTTP/RPC, UDP/KCP, or a production shutdown coordinator.

## Verification

- `tests/integration/game_pipeline/test_game_server_pipeline.cpp` verifies real
  TCP connect, segmented authentication, sticky command frames, logic responses,
  and disconnect session cleanup.
- `tests/integration/game_pipeline/test_game_server_pipeline_auth_state.cpp`
  uses example-only friend seams to prove a 130-frame I/O result is delivered
  as deferred owner-loop batches of 64/64/2 without reordering and to inject
  AUTH plus command in one exact `onFrames` batch. It separately observes
  distinct I/O, management, logic, and endpoint stages across three physical
  loops, and verifies duplicate-player rejection discards queued pre-auth
  commands.
- `tests/integration/game_pipeline/test_game_server_pipeline_shutdown.cpp`
  verifies stop with a delayed-auth TimerId pending, holds an active worker I/O
  callback across Pipeline member teardown, synchronously stops from the
  Logic-stage observer without destroying the active tick, deterministically
  holds a distinct-loop Logic callback while management-side stop queues owner
  teardown and waits for the callback to return, cancels an endpoint send whose
  observer spans revocation, and verifies one-shot start. The delayed-auth seam
  proves both TimerId removal and immediate release of the timer-owned attempt.
  Shutdown convergence is cancellation-based rather than a completion future
  for every transport close.
- `tests/integration/game_pipeline/test_game_server_pipeline_handoff.cpp`
  verifies worker-I/O handoff, disconnect-before-auth completion, repeated AUTH
  rejection, independent pending-auth frame-count and byte-budget overflow, and
  session cleanup.

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: TCP-to-framer-to-session-to-logic-to-endpoint composition,
  owner-loop handoff, and disconnect cleanup.
- Deferred from the source design: production auth, multi-protocol adapters,
  business state, and a formal installed Pipeline library remain out of scope.
- Dropped behavior: none; explicit authentication transitions and callback
  lifetime revocation are restored in the example hardening contract.
