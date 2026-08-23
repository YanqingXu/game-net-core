# M8 Async / Coroutine Promotion Audit

Date: 2026-08-24

Core baseline: `e5ea9efa71dbe52e841423ec3cac3e9529158b22`

## Decision

- shared async/coroutine promotion: `NO-PROMOTION`;
- `async_semantics`, `coroutine_task`, `async_timer`,
  `connection_awaiter_registry`, `when_all`, and `when_any`: remain
  `deferred`;
- no Core coroutine header, target, package component, compatibility promise,
  tag, empty v0.7 release, or callback API replacement is created;
- M8 is closed and the unique governance front advances to M9 TLS,
  WebSocket, and DNS evidence review.

The evidence proves several bounded asynchronous implementations, but it does
not prove two consumers of one substitutable value/error/cancel/resume,
executor, frame-ownership, timer-retirement, or connection-await contract.

## Audited Inputs

### Current GameNet Core

Core has no coroutine runtime. Its current reusable substrate is callback-based:

- `EventLoopExecutor` is a copyable, non-owning capability for one EventLoop;
  `post()` returns `Accepted`, `QueueFull`, `Shutdown`, or
  `OwnerUnavailable` and accepted callbacks run on that loop owner;
- `TimerScheduleResult` commits callback timer metadata only when Accepted;
  EventLoop shutdown cancels future timers instead of waiting for their
  deadlines;
- EventLoop final drain covers accepted functors and committed internal
  lifecycle work, but owns no suspended coroutine frame or continuation token;
- TcpConnection remains callback-based and has no read/write/close awaiter
  registry.

The current build passed the directly relevant contracts 3/3:

- `contract.event_loop.test_event_loop`;
- `contract.timer_queue.test_timer_queue`;
- `contract.timer_queue.test_deadline_queue`.

These tests prove the existing callback/executor/timer contract. They are not
coroutine promotion evidence.

### `gamenet-game-gateway`

Gateway implementation `e43393c85fa37604d340fe866610756c99f4fe4e` and
closure `588acd079be93de3e230ba4f07dd111f7bec6a3c` deliberately expose a
callback/value typed-RPC adapter:

- `CompletionCallback`, `CallAdmissionCallback`, and `CancelResultCallback`
  deliver values and typed terminal/admission results;
- each per-connection Channel and its timers stay on the network EventLoop;
- the distinct Lua cell runs copied handler values on its private owner and
  posts a responder back to the exact network owner;
- no `Task`, `co_await`, connection awaiter, async sleep, `when_all`, or
  `when_any` runtime exists in the implementation;
- M7 explicitly required the adapter to remain useful without coroutine.

Gateway therefore proves that the current real GameNet consumer does not need
a public coroutine API.

### `YanGameServer`

The independent source checkpoint is
`b5254165389d762c3f3c63568c24ffab448fc501`. It was inspected and built from
the existing read-only archive, without switching or modifying its live
checkout. Its focused Windows Release contract-fallback set passed 9/9:

- `yangame-persistence-executor`;
- `yangame-persistence-executor-repository`;
- `yangame-persistence-executor-flow`;
- `yangame-persistence-executor-race`;
- `yangame-coroutine-task`;
- `yangame-coroutine-race`;
- `yangame-timer`;
- `yangame-lua-rpc-await`;
- `yangame-actor-rpc-flow`.

YanGame's coroutine is an Actor runtime contract, not an EventLoop utility:

- a shared Task control state singularly owns the raw coroutine frame;
- `ActorScheduler::startTask` admits a declared logical frame charge into
  bounded active-frame and ready-token capacity;
- every initial and continued resume revalidates one generation-bearing
  ActorRef and executes on that Actor shard owner;
- `RpcPendingRequest` stores one narrow continuation token; a terminal result
  is published before the bounded token is queued, so completion never resumes
  inline;
- Actor drain, replacement, cancellation, and shard shutdown cancel and
  destroy admitted frames on the Actor owner;
- the public Task has no detached fire-and-forget contract and is not the
  general `start()` / `detach()` / Task-to-Task `operator co_await` wrapper
  described by Core's old deferred intent;
- `ActorTimer` owns an Actor message completion and waiter observation, while
  RPC await cancels that timer on the Actor owner; it is not Core's proposed
  EventLoop `SleepAwaitable`;
- `PersistenceExecutor` is a separate bounded blocking worker pool with
  request/wait/cancel semantics, not the Task executor.

This is valid consumer-specific evidence, but copying it into Core would make
GameNet depend on YanGame's ActorRef, shard, mailbox, logical-frame budget,
message timer, and `Result<T>` semantics.

## Contract Comparison

| Boundary | Core / gateway | YanGameServer | M8 conclusion |
| --- | --- | --- | --- |
| Result carrier | Core `PostResult` / `TimerScheduleResult`; gateway `CallResult` plus callback `Completion` | `Result<T>` with project ErrorCode, Task result waiting, and RPC await | Success/error classes overlap conceptually; carriers and completion contracts differ |
| Executor owner | EventLoop callback owner; gateway additionally owns a private Lua cell | Actor shard owns Task admission/resume; persistence owns separate workers | No single executor or resume target |
| Frame ownership | No coroutine frame in Core or gateway | Shared Task control state singularly owns frame; Actor runtime retains it after admission | Only one consumer has a frame contract |
| Start/detach | Not applicable | Lazy start only through bounded `ActorScheduler::startTask`; no detached facility | Core deferred `start/detach` model is not evidenced |
| Cancellation | Typed EventLoop admission and callback terminal settlement | Actor/RPC terminal publication queues one token; Actor lifecycle destroys on owner | Same exactly-once goal, different owner and retirement mechanism |
| Timer | EventLoop callback metadata; future timers are discarded at shutdown | Actor message timer with completion/waiter state and owner cancellation | No substitutable async-sleep contract |
| TCP awaiters | None; real gateway remains callback-based | RPC-specific pending awaiter, not a GameNet TcpConnection registry | `connection_awaiter_registry` has zero consumer pair |
| Composition | No `when_all` / `when_any` runtime | No `when_all` / `when_any` runtime | No implementation evidence |
| Lua bridge | Gateway callback/value cell, no Lua coroutine | YanGame-specific Luax/YanLua owner and RPC coroutine association | Different VM and completion boundaries; remains external |

## Deferred Intent Review

The six preserved intents are migration design assets, not current
authorization:

1. `async_semantics` assumes a shared `std::expected<T, AsyncError>` direction,
   while the audited consumers use three different typed result families.
2. `coroutine_task` describes caller-driven `start()`, `detach()`, Task-to-Task
   await, and self/awaiter frame transfer. YanGame instead requires bounded
   Actor admission and one shared control-state owner; gateway has no Task.
3. `async_timer` permits EventLoop exit to leave a handle unresumed. That is
   incompatible with M8's zero-suspended-frame shutdown gate and cannot be
   activated without rewriting its lifecycle contract.
4. `connection_awaiter_registry` has no implementation in either audited
   consumer and would add TcpConnection lifetime coupling without demand.
5. `when_all` lets the last completing subtask choose the parent resume thread
   and treats parent destruction while children run as a scenario to avoid.
   This does not satisfy the current explicit owner-resume/lifetime gate.
6. `when_any` similarly lets the winner choose the parent thread and assumes
   cooperative loser cancellation that the audited Task contracts do not
   share.

Any future resume must first rewrite the affected intent against current Core,
promote its metadata to `active`, name concrete owner/frame/cancel/shutdown
contracts, and provide two real consumers plus direct contract tests.

## Ownership, Re-entry, and Cross-Thread Decision

This audit creates no new runtime owner and changes no callback permission.
Existing EventLoop, gateway network/Lua, and YanGame Actor/persistence owners
remain separate. Gateway completion callbacks may continue to re-enter its
typed facade; YanGame Actor handlers and Task continuation paths retain their
own re-entry rules. Core cross-thread work continues through
`EventLoopExecutor` or existing typed lifecycle facilities; no coroutine
handle becomes a cross-thread mutation capability.

## Repository Verification

`tests/cmake/test_migration_status_contract.py` binds this audit, all six
deferred intent/catalog entries, the external checkpoints and 3/3 plus 9/9
evidence, M8 `NO-PROMOTION`, M9 as the next governance front, and the absence
of installed Core coroutine/awaiter headers or CMake targets.
