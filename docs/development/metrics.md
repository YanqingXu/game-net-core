# Provisional Production Metrics

The optional metrics layer is installed through `GameNet::core`, but its public
API remains provisional in the 0.3 candidate line. It provides a
thread-safe in-memory exporter, immutable static labels, deterministic value
snapshots, Prometheus text rendering, and callback adapters for the metric
hooks already owned by Core, Logic, and Broadcast.

## Runtime Boundary

Metric callbacks execute synchronously on the original producer thread. The
recorders do not move work, own loops or connections, perform network I/O, or
call back into reactor modules. They capture shared exporter ownership directly
and contain exporter exceptions so a failed observability sink cannot change
connection, admission, logic-tick, or broadcast behavior.

`InMemoryMetricsExporter` serializes concurrent updates with its own mutex.
Call `snapshot()` from a reporting thread and render that value copy with
`renderPrometheusText()`. Rendering is intentionally outside hot owner-loop
callbacks. A future network endpoint should serve these snapshots from an
upper-layer reporting component rather than add HTTP behavior to Core.

The reference exporter also validates names, constructs string keys, hashes
maps, and takes a shared mutex on every observation. It is suitable for tests,
local diagnostics, and functional integration, not yet a proven high-rate
production recorder. Stable promotion requires pre-registered metric
identifiers, sharded aggregation, type-conflict validation, and a dedicated
metrics-on/off performance gate.

## Cardinality Boundary

`TaggedMetricsExporter` accepts only static deployment labels. Labels are
validated, sorted, escaped, and encoded deterministically. Recorder adapters
do not emit peer addresses, connection names, player IDs, session IDs, or
transport IDs as labels; those values would create unbounded production
cardinality.

Core recorder prefixes:

- `gamenet.net.connector.*`
- `gamenet.net.event_loop.*`
- `gamenet.net.tcp_server.admission.*`

Upper-layer recorder prefixes:

- `gamenet.game_logic.*`
- `gamenet.broadcast.*`

Tests cover aggregation/reset, label validation and escaping, deterministic
Prometheus snapshots, callback lifetime, exporter-failure containment,
concurrent recording, owner-loop EventLoop recording, and a real
LogicLoop/Broadcast integration chain.

