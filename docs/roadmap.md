# Roadmap

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

## Phase 4: Higher-level Modules

- Add protocol framing.
- Add transport abstraction.
- Add session, logic-loop, and broadcast foundations.
- Keep UDP/KCP work under experimental until the stable core is proven.
