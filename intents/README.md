# Intent Index

This directory preserves the intent-driven development workflow inherited from
the original `mini_trantor` repository and adapts it for `game-net-core`.
The repository exists to split and migrate that larger project by component
boundary, starting with the Reactor / TCP foundation.

Intent files are design contracts. They describe why a module exists, which
invariants it must preserve, what it must not do, and which tests prove the
contract. Code changes should be derived from these files, not invented from
implementation details first.

## Metadata Contract

Every `*.intent.md` document starts with this authoritative front matter:

```yaml
---
status: active | deferred | legacy
target: GameNet::core | GameNet::protocol | GameNet::transport | GameNet::game | GameNet::experimental | historical
migration_source: mini_trantor | native
promote_gate: none | phase-4-protocol | phase-4-transport | phase-4-game-foundation | post-core-preview | post-phase-4-protocol | experimental-only | never
---
```

Status semantics:

- `active` is current implementation authorization. It must target
  `GameNet::core`, use `promote_gate: none`, and appear in Active Intents.
- `deferred` is a future design asset only. Its body may preserve source-project
  names, options, test paths, or historical sequencing, but none of those are
  current repository facts. Promotion requires its named gate, a body rewrite
  against current code, matching rules/contracts/tests, and an index change.
- `legacy` is historical context that must not be promoted or used as current
  scope, roadmap, build, or validation evidence. Replace it with a new intent
  if the idea becomes relevant again.

The catalogs below must contain every formal intent exactly once and must agree
with front matter. Files named `*.intent.template.md` are scaffolding rather
than formal intents and are intentionally outside the catalogs.

## Required Workflow

Before changing an important module:

1. Read the relevant intent file under `intents/`.
2. Read the relevant rules under `rules/`.
3. Identify invariants, threading rules, ownership rules, and public contracts.
4. Add or update unit / contract / integration tests in the same change.
5. Answer the core-module change gate in the change description.

Core-module change gate:

- Which loop/thread owns this module?
- Who owns it and who releases it?
- Which callbacks may re-enter?
- Which operations are allowed cross-thread, and how are they marshaled?
- Which specific test file verifies this change?

## Active Intents

These intents apply to the current migrated core:

- `intents/usecases/core_performance_baseline.intent.md`
- `intents/modules/logger.intent.md`
- `intents/modules/channel.intent.md`
- `intents/modules/buffer.intent.md`
- `intents/modules/acceptor.intent.md`
- `intents/modules/connector.intent.md`
- `intents/modules/tcp_connection.intent.md`
- `intents/modules/tcp_server.intent.md`
- `intents/modules/tcp_client.intent.md`
- `intents/modules/event_loop.intent.md`
- `intents/modules/event_loop_thread.intent.md`
- `intents/modules/event_loop_thread_pool.intent.md`
- `intents/modules/platform_runtime.intent.md`
- `intents/modules/poller.intent.md`
- `intents/modules/timer_queue.intent.md`
- `intents/architecture/lifetime_rules.intent.md`
- `intents/architecture/threading_model.intent.md`
- `intents/usecases/echo_server.intent.md`

## Deferred Intent Catalog

These design assets do not authorize expanding current scope:

- `intents/modules/async_semantics.intent.md`
- `intents/modules/async_timer.intent.md`
- `intents/modules/backpressure_policy.intent.md`
- `intents/modules/connection_awaiter_registry.intent.md`
- `intents/modules/connection_backpressure_controller.intent.md`
- `intents/modules/connection_transport.intent.md`
- `intents/modules/coroutine_task.intent.md`
- `intents/modules/dns_resolver.intent.md`
- `intents/modules/game_backpressure_policy.intent.md`
- `intents/modules/game_gateway_security.intent.md`
- `intents/modules/graceful_shutdown.intent.md`
- `intents/modules/http.intent.md`
- `intents/modules/kcp_transport.intent.md`
- `intents/modules/metrics_exporter.intent.md`
- `intents/modules/path_mtu_cache.intent.md`
- `intents/modules/path_mtu_signal_authentication.intent.md`
- `intents/modules/platform_path_mtu_signal.intent.md`
- `intents/modules/rpc.intent.md`
- `intents/modules/tls.intent.md`
- `intents/modules/udp.intent.md`
- `intents/modules/websocket.intent.md`
- `intents/modules/when_all.intent.md`
- `intents/modules/when_any.intent.md`

## Legacy Intent Catalog

These documents record the source project's historical sequencing and must not
be treated as current `game-net-core` plans or evidence:

- `intents/architecture/game_network_base_scope.intent.md`
- `intents/architecture/system_overview.intent.md`
- `intents/architecture/v1_stages.intent.md`
- `intents/architecture/v2_stages.intent.md`
- `intents/architecture/v3_stages.intent.md`
- `intents/architecture/v5_delta_config_observability.intent.md`
- `intents/architecture/v5_epsilon_protocol_transport_decoupling.intent.md`
- `intents/architecture/v5_stages.intent.md`
- `intents/architecture/v5_zeta_engineering_guardrails.intent.md`
- `intents/architecture/v6_alpha_client_ecosystem.intent.md`
- `intents/architecture/v6_stages.intent.md`
