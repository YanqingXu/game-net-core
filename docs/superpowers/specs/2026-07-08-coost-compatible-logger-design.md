# Coost-Compatible Logger Design

## Goal

Rework the existing `gamenet::base::Logger` into a C++23, coost-inspired
compatibility layer while preserving the current `LOG_TRACE`, `LOG_DEBUG`,
`LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL`, `LOG_SYSERR`, and
`LOG_SYSFATAL` call sites used by the Reactor/TCP core.

This design intentionally implements the compatible enhanced version approved
for this step. It does not introduce an asynchronous logging thread, buffered
background flushing, file rotation, signal handlers, or stack trace dumping.

## Intent And Rules

Relevant repository intent:

- `intents/architecture/system_overview.intent.md`
- `intents/architecture/game_network_base_scope.intent.md`
- `intents/architecture/threading_model.intent.md`
- `intents/architecture/lifetime_rules.intent.md`
- `rules/thread_affinity_rules.md`
- `rules/ownership_rules.md`
- `rules/testing_rules.md`
- `rules/coding_rules.md`

Logger is base infrastructure. It is not owned by an `EventLoop`, does not own
reactor state, and must not add a hidden cross-thread mutation path for core
networking objects. Each log record is formatted by the calling thread and then
sent to configured callbacks under logger-internal synchronization.

## Public API

Keep the existing public surface:

- `gamenet::base::Logger`
- `Logger::LogLevel`
- `Logger::setLogLevel()`
- `Logger::setOutputFunction()`
- `Logger::setFlushFunction()`
- `gamenet::base::logLevel()`
- `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`,
  `LOG_FATAL`, `LOG_SYSERR`, `LOG_SYSFATAL`

Add coost-style convenience API:

- `DLOG`, `LOG`, `WLOG`, `ELOG`, `FLOG`
- `DLOG_IF`, `LOG_IF`, `WLOG_IF`, `ELOG_IF`, `FLOG_IF`
- `DLOG_EVERY_N`, `LOG_EVERY_N`, `WLOG_EVERY_N`, `ELOG_EVERY_N`
- `DLOG_FIRST_N`, `LOG_FIRST_N`, `WLOG_FIRST_N`, `ELOG_FIRST_N`
- `TLOG(topic)` and `TLOG_IF(topic, condition)`
- `CHECK`, `CHECK_NOTNULL`, `CHECK_EQ`, `CHECK_NE`, `CHECK_GE`, `CHECK_LE`,
  `CHECK_GT`, `CHECK_LT`
- `Logger::setTopicOutputFunction()`
- `Logger::resetForTesting()`

The new short macro names are intentionally added because the user asked for a
coost-style logging experience. Existing `LOG_*` names remain valid so current
core code does not need a broad mechanical migration.

## Formatting

Level logs use a coost-style compact prefix:

```text
I0708 11:42:33.123 thread-id file.cc:42] message
```

The first character is `T`, `D`, `I`, `W`, `E`, or `F` for trace, debug, info,
warning, error, or fatal. Time uses local time with millisecond precision.
`file.cc` is the basename of `__FILE__`.

Topic logs use the same timestamp, thread id, basename, and line fields, with a
topic marker before the user message:

```text
I0708 11:42:33.123 thread-id file.cc:42] [topic:net] accepted connection
```

Topic output callbacks receive the topic separately as well as the formatted
bytes. If no topic callback is configured, topic logs fall back to the normal
output callback.

## Threading

Logger has process-global configuration, independent of `EventLoop` ownership.

- Setting log level and callbacks is thread-safe.
- Emitting a log is thread-safe.
- A log record is formatted on the caller's thread.
- Output callbacks are called after copying the configured callback under a
  mutex, so callback invocation does not hold logger configuration locks.
- Logging does not marshal through `EventLoop`; it must not mutate loop-owned
  state.

## Ownership

The logger owns no reactor resources. It stores copies of callback function
objects. Callers own any state captured by those callbacks and must ensure their
lifetime is valid while callbacks remain installed.

`Logger` instances are temporary record builders owned by the expression that
created them. Their destructor emits the final line. `FATAL` and failed
`CHECK` records flush and abort after output.

## Failure Semantics

- Suppressed logs must not evaluate the streamed expression.
- `FLOG` always logs and aborts.
- Failed `CHECK*` logs a fatal message and aborts.
- `LOG_SYSERR` and `LOG_SYSFATAL` append errno/system error context at record
  construction time.
- `Logger::resetForTesting()` restores level, output callback, topic callback,
  and flush callback to deterministic defaults for tests.

## Tests

New tests live under:

- `tests/unit/base/test_logger.cpp`
- `tests/contract/base/test_logger_contract.cpp`

Unit tests verify:

- existing `LOG_INFO` output still works
- coost-style short macros emit the expected message and prefix marker
- log-level filtering avoids expression evaluation
- conditional logs avoid expression evaluation when false
- `EVERY_N` and `FIRST_N` counters gate output
- topic logs pass the topic to the topic callback

Contract tests verify:

- `CHECK_EQ` failure exits through a child process
- `LOG_SYSERR` includes system error context
- callbacks can be replaced and reset without leaking state across tests

## Core-Module Change Gate

- Which loop/thread owns this module?
  - No `EventLoop` owns Logger. Logger is process-global base infrastructure.
- Who owns it and who releases it?
  - Static logger configuration owns callback copies. Temporary `Logger`
    records are expression-scoped and release themselves after emitting.
- Which callbacks may re-enter?
  - Output, topic output, and flush callbacks are user-provided and may log
    again. Logger copies callbacks before invocation to avoid holding internal
    locks during callback execution.
- Which operations are allowed cross-thread, and how are they marshaled?
  - Log emission and logger configuration are thread-safe through internal
    synchronization. There is no `EventLoop` marshaling because Logger does not
    mutate reactor-owned state.
- Which specific test file verifies this change?
  - `tests/unit/base/test_logger.cpp`
  - `tests/contract/base/test_logger_contract.cpp`
