---
status: active
target: GameNet::core
migration_source: mini_trantor
promote_gate: none
---

# Module Intent: Buffer

## 1. Intent
Buffer is the byte container used by connection read/write paths.
It provides stable readable/writable/prependable regions and a narrow fd I/O bridge
without owning protocol semantics.

---

## 2. Responsibilities
- store inbound and outbound bytes for one connection path
- expose readable/writable/prependable byte accounting
- support append/retrieve operations with predictable index movement
- support scatter-read style growth for efficient socket reads
- accept a caller-supplied per-read byte ceiling so a connection can enforce
  input admission before Buffer growth
- bound historical retained storage independently from active readable bytes:
  capacity may exceed the configured retention target while active data needs
  it, then is trimmed only after readable bytes fall to the configured lower
  threshold

---

## 3. Non-Responsibilities
- does not own socket or EventLoop
- does not parse framing or protocol boundaries
- does not define backpressure policy

---

## 4. Core Invariants
- readableBytes, writableBytes, and prependableBytes stay internally consistent
- append never invalidates existing readable data semantics
- pointer/range APIs require `data` to denote a readable `[data, data + len)`
  range when `len > 0`; zero-length append is a no-op and permits null
- `retrieveUntil(end)` requires `end` inside the current inclusive readable
  range, and `hasWritten(len)` requires `len <= writableBytes()`
- retrieve moves reader state forward or resets cleanly
- fd read/write helpers report explicit errno on failure
- a bounded fd read never appends more than the caller-provided byte ceiling
- trim preserves every readable byte and resets it after the cheap-prepend
  region
- trim is hysteretic: exceeding the retention target arms it, staying above the
  lower readable threshold keeps capacity, and reaching the lower threshold
  performs at most one shrink until a later oversized growth

---

## 5. Threading Rules
- Buffer is not internally synchronized
- one connection loop should own mutation of a given Buffer instance
- retention snapshots and explicit trim attempts are owner-thread observations,
  not cross-thread diagnostics

---

## 6. Failure Semantics
- fd I/O failure is reported to caller via return value and saved errno
- growth strategy should stay explicit rather than silently dropping bytes
- a zero per-read ceiling performs no socket read and no Buffer growth
- retention trim is opportunistic; allocation failure keeps the old valid
  storage armed for a later attempt and never changes readable bytes

---

## 7. Test Contracts
- append/retrieve preserve byte ordering
- makeSpace keeps unread data intact
- readFd grows into extra buffer path correctly
- readFd honors an exact caller-provided maximum without consuming beyond it
- writeFd exposes explicit error reporting
- historical oversized growth trims to the configured retained-capacity target
  only after the recovery threshold, preserves unread data, and does not
  repeatedly trim during subsequent small append/retrieve cycles

---

## 8. Review Checklist
- Are index transitions still easy to reason about?
- Does growth preserve existing unread bytes?
- Is Buffer remaining protocol-agnostic?
