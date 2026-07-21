---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: Platform Runtime Boundary

## Intent

Platform runtime boundary isolates operating-system differences required by the
reactor core while keeping `EventLoop`, `Channel`, `TcpConnection`, and upper
layers backend-neutral.

Linux and Windows may use different socket handles, wakeup primitives, and I/O
backends, but they must expose the same owner-loop scheduling semantics to
upper layers.

This module is not a scheduler.
This module is not a connection lifecycle owner.
This module is not business logic.

---

## Responsibilities

- Define platform socket handle aliases through `gamenet/core/net/platform/SocketTypes.h`.
- Implement `SocketsOps` behind the stable public `gamenet/core/net/SocketsOps.h` entry.
- Create, write, drain, and close EventLoop wakeup descriptors per platform.
- Host concrete poller backends under `gamenet/core/net/poller/`.
- Select the default poller backend at build time through `Poller::newDefaultPoller()`.
- Keep unsupported platform capabilities explicit.

---

## Non-Responsibilities

- Does not own `EventLoop`, `Channel`, `Socket`, or `TcpConnection`.
- Does not invoke user callbacks.
- Does not choose thread-pool scheduling or connection placement.
- Does not hide cross-thread operations from `EventLoop::queueInLoop()`.
- Does not make Linux-only PMTU/signalfd features available on Windows.

---

## Core Invariants

- Public reactor semantics stay identical across Linux and Windows.
- `EventLoop` owns wakeup descriptors and closes them after removing the wakeup Channel.
- `Poller` observes `Channel` and never owns it.
- Platform socket functions never transfer ownership implicitly.
- Runtime TCP socket creation, bind, listen, and local/peer-address discovery expose
  non-fatal result APIs; Reactor/TCP owners must not use process-terminating
  wrappers for failures that can be isolated to one listener or connection.
- Linux stream writes suppress per-call `SIGPIPE` delivery and report peer-close
  failure through the normal return value / `errno` path without changing the
  embedding process's signal disposition.
- The shared Linux write helper preserves ordinary descriptor semantics for
  pipes, while the eventfd wakeup hot path uses `write(2)` directly; socket
  writes still use per-call `MSG_NOSIGNAL`.
- Platform-specific code must not leak backend event constants into user-facing APIs.
- Compatibility headers may forward old include paths, but implementation belongs in
  `platform/` or `poller/`.

---

## Collaboration

- `EventLoop` calls `platform::createWakeupFds()`, `writeWakeup()`, and `drainWakeup()`.
- `Socket`, `Acceptor`, `Connector`, `TcpConnection`, and future promoted
  platform users call `sockets::*` through the stable public header.
- `PollerFactory` chooses an IOCP-backed Windows backend and `EPollPoller` on Linux.
- CMake selects exactly one socket implementation, one wakeup implementation, and
  one concrete poller backend for the target platform.

---

## Threading Rules

- Platform wakeup functions are part of the EventLoop cross-thread scheduling path.
- Descriptor creation and close are owned by the EventLoop owner thread lifecycle.
- Cross-thread wakeup writes are allowed only through `EventLoop::wakeup()`.
- Poller backend mutation remains owner-thread only.
- Synchronous capability queries may be thread-neutral only when they do not touch
  loop-owned descriptors.

---

## Ownership Rules

- `EventLoop` owns wakeup descriptors.
- `Socket` owns `SocketFd` unless `releaseFd()` explicitly transfers it.
- `Poller` or an equivalent platform backend owns only its backend kernel
  object, not `Channel`.
- `SocketsOps` functions observe descriptors passed by callers unless the function
  name explicitly creates or closes one.
- Windows WinSock process initialization is owned by the platform socket runtime.

---

## Failure Semantics

- Process-wide runtime initialization and Poller/wakeup construction may fail
  fast when no EventLoop can operate safely.
- Listener and connection socket creation, bind, listen, accepted-socket setup,
  and connector setup return an invalid handle, status, or captured platform
  error so the owning module can reject, retry, or stop locally.
- `*OrDie` compatibility helpers are not valid in the recoverable Reactor/TCP
  runtime path.
- Unsupported platform features must be reported as unsupported, not emulated
  silently.
- Would-block and interrupted errors must normalize to stable helper predicates.
- A Linux write after peer close returns an explicit socket error; it must not
  terminate the hosting process through `SIGPIPE`.
- Wakeup drain failure is logged by `EventLoop` without changing callback ordering.

---

## Test Contracts

- `tests/contract/event_loop/test_event_loop.cpp` verifies wakeup and queued functor
  semantics.
- `tests/contract/poller/test_poller_contract.cpp` verifies backend-neutral poller
  registration behavior when enabled on the platform.
- TCP client/server/connection contract tests verify socket operation behavior
  through the public path.
- `tests/contract/socket/test_socket_contract.cpp` runs the Linux peer-close
  write path in a child with default `SIGPIPE` disposition and verifies an
  `EPIPE` result plus normal process exit.
- `tests/contract/socket/test_socket_contract.cpp` also verifies invalid socket
  creation, bind, and listen failures are returned without terminating the
  process.
- Future Windows workflow verifies the Windows source selection and IOCP completion path.
- `docs/development/windows_iocp_milestone.md` defines the Windows promotion
  gates for IOCP wakeup, overlapped read/write ownership, and cancel/close ordering.
- Linux CI/builds verify the Linux source selection and EPollPoller path.

---

## Review Checklist

- Does the change keep platform code under `gamenet/core/net/platform/` or
  `gamenet/core/net/poller/`?
- Does the public include path remain stable when practical?
- Which loop/thread owns the affected descriptors?
- Who owns and releases each descriptor/backend handle?
- Which callbacks may re-enter after platform events are translated?
- Which operations are allowed cross-thread, and are they still marshaled by EventLoop?
- Which test file verifies the behavior on each platform?
