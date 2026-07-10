# Roadmap

game-net-core is the component-split migration target for `mini_trantor`.
The roadmap keeps that migration staged so the networking core becomes stable
before protocol, transport, game-foundation, or experimental modules are added.
See `migration_status.md` for the current checked state of these phases.

## Phase 1: Project Skeleton

- Initialize CMake, README, repository rules, and documentation structure.
- Establish public header and implementation layout.

## Phase 2: Reactor / TCP Core

- Migrate Logger, Timestamp, noncopyable, socket primitives, Channel, Poller,
  Wakeup, TimerQueue, EventLoop, and TCP lifecycle components.
- Keep the first target focused on `gamenet_core`.

## Phase 3: Targets and Tests

- Split unit, contract, and integration tests.
- Add minimal echo-server coverage for the TCP path.
- Export and install `GameNet::core` so downstream components can consume the
  split package through `find_package(GameNetCore)`.

## Phase 3.5: Core Preview Hardening

Completed in the current worktree:

- make `TcpConnection::connected()` / `disconnected()` atomic snapshot observers;
- make TcpConnection callback, context, and socket-option mutation owner-loop-only;
- marshal TcpServer callback installation/replacement to each connection owner loop;
- fix the Linux repeated-connect failure found in `ci` run `29059799283` by
  releasing Connector member ownership before deferred Channel destruction;
- define Logger runtime replacement, callback snapshot/concurrency, re-entry,
  and capture-lifetime semantics with a threading contract;
- add a default-off, non-CTest core benchmark with versioned JSON output for
  echo throughput/latency, connection working-set growth, worker-loop scaling,
  and slow-client accumulation;
- record the first local Windows MSVC Release baseline, including the current
  single-completion IOCP mode and raw JSON evidence;
- classify all 52 formal intents with machine-checked active/deferred/legacy
  metadata so historical mini_trantor stages and test counts cannot authorize
  current work;
- replace self-referential HEAD documentation with immutable validation records;
- pass local Windows Debug, Release, install-consumer, and 5-repeat threading preflight.

Remaining gates, in order:

1. Commit the focused Phase 3.5 changes and obtain one green `ci` run where all
   five jobs validate the same commit.
2. Run remote `long-soak` with the current 46-test threading slice at repeat 50
   and record run id, commit, date, timeout, result, and duration.
3. Dispatch the manual `core-benchmark` workflow on the focused commit and
   retain its same-SHA Linux epoll and Windows IOCP Release JSON artifacts.
4. Review the complete CI, soak, and cross-platform benchmark evidence ledger,
   then tag `v0.1.0-core-preview`.

## Phase 4: Higher-level Modules

Phase 4 must not begin until the Phase 4 readiness gate in
`docs/migration_status.md` is satisfied with current evidence.

- Add protocol framing.
- Add transport abstraction.
- Add game foundation pieces such as session, logic-loop, and broadcast support.
- Keep UDP/KCP work under experimental until the stable core is proven.
