# Core Reactor Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first compilable `gamenet_core` Reactor foundation from the ignored `mini_trantor` source tree.

**Architecture:** Copy only base and Reactor files into `include/gamenet/core` and `src/core`. Rewrite includes from `mini/...` to `gamenet/core/...` and namespaces from `mini::base` / `mini::net` to `gamenet::base` / `gamenet::net`. Keep TCP server/client/connection out of this batch.

**Tech Stack:** C++23, CMake, CTest, platform socket APIs.

---

### Task 1: Add Core Contract Tests

**Files:**
- Create: `tests/unit/channel/test_channel.cpp`
- Create: `tests/contract/event_loop/test_event_loop.cpp`
- Create: `tests/contract/timer_queue/test_timer_queue.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] Write tests that include `gamenet/core/...` headers and use `gamenet::base` / `gamenet::net`.
- [ ] Run CMake or the available compiler command and verify tests fail because the new headers are missing.
- [ ] Keep the tests focused on Channel, EventLoop, EventLoopThread, and TimerQueue behavior.

### Task 2: Migrate Base and Reactor Headers

**Files:**
- Create: `include/gamenet/core/base/*.h`
- Create: `include/gamenet/core/net/*.h`
- Create: `include/gamenet/core/net/platform/*.h`
- Create: `include/gamenet/core/net/poller/*.h`

- [ ] Copy only the approved base and Reactor headers from `mini_trantor/mini`.
- [ ] Rewrite include paths to `gamenet/core/...`.
- [ ] Rewrite namespaces to `gamenet::base` and `gamenet::net`.
- [ ] Add a small local EventLoop metrics header instead of importing old broad `MetricsHook.h`.

### Task 3: Migrate Base and Reactor Implementations

**Files:**
- Create: `src/core/base/*.cc`
- Create: `src/core/net/*.cc`
- Create: `src/core/net/platform/*.cc`
- Create: `src/core/net/poller/*.cc`
- Modify: `src/core/CMakeLists.txt`

- [ ] Copy only the approved implementation files from `mini_trantor/mini`.
- [ ] Rewrite include paths and namespaces.
- [ ] Wire the files into the `gamenet_core` CMake target.
- [ ] Keep platform-specific socket and wakeup files selected by `WIN32`.

### Task 4: Verify and Commit

**Files:**
- Modify as needed only inside migrated core files and tests.

- [ ] Run whitespace checks with `git diff --check`.
- [ ] Run CMake configure/build/tests when `cmake` is available.
- [ ] If CMake is unavailable, record that verification gap and run all possible repository-local checks.
- [ ] Commit with `feat(core): add reactor event loop foundation`.
