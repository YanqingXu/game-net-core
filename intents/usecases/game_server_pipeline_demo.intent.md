---
status: active
target: GameNet::game
migration_source: native
promote_gate: none
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

- The example uses one explicit loop for TCP I/O, session management, and logic
  ticks while still exercising asynchronous queue boundaries.
- Per-connection framing state is owned by the connection's loop-side demo
  state. Disconnect removes it and asynchronously removes the matching session.
- Session callbacks and logic output callbacks may re-enter the composition on
  the owner loop; no queue lock is held.
- Output is always queued to the endpoint owner loop before `send`.

## Scope

- `GameServerPipeline` remains in `examples/game_server_pipeline_demo` and is
  neither installed nor exported.
- The demo does not define account auth, message headers/codecs, player business
  state, HTTP/RPC, UDP/KCP, or a production shutdown coordinator.

## Verification

`tests/integration/game_pipeline/test_game_server_pipeline.cpp` verifies real
TCP connect, segmented authentication, sticky command frames, logic responses,
and disconnect session cleanup.
