# Intent Index

This directory preserves the intent-driven development workflow inherited from
the original repository and adapts it for `game-net-core`.

Intent files are design contracts. They describe why a module exists, which
invariants it must preserve, what it must not do, and which tests prove the
contract. Code changes should be derived from these files, not invented from
implementation details first.

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

- `intents/modules/channel.intent.md`
- `intents/modules/buffer.intent.md`
- `intents/modules/acceptor.intent.md`
- `intents/modules/connector.intent.md`
- `intents/modules/event_loop.intent.md`
- `intents/modules/event_loop_thread.intent.md`
- `intents/modules/event_loop_thread_pool.intent.md`
- `intents/modules/poller.intent.md`
- `intents/modules/timer_queue.intent.md`
- `intents/architecture/lifetime_rules.intent.md`
- `intents/architecture/threading_model.intent.md`

## Deferred Intents

The remaining intents are preserved as design assets for later phases. They do
not authorize expanding the current scope by themselves. `TcpConnection`,
`TcpServer`, `TcpClient`, HTTP, WebSocket, RPC, TLS, UDP, KCP, DNS, coroutine,
game-session, broadcast, and metrics work should stay deferred until the roadmap
explicitly promotes that module into the active migration scope.
