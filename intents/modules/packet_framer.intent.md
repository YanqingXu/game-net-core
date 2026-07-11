---
status: active
target: GameNet::protocol
migration_source: mini_trantor
promote_gate: none
artifact_kind: installed-library
migration_mode: redesign
source_commit: 3eba368475a68f677aae920d4f299b155db23d57
source_paths: mini/net/framing/PacketFramer.h;mini/net/framing/PacketFramer.cc;tests/fuzz/framing/fuzz_packet_framer.cpp
---

# Module Intent: PacketFramer

## Intent

`GameNet::protocol` needs one transport-neutral primitive that converts an
arbitrarily segmented byte stream into length-delimited payloads. The first
format is exactly `uint32 big-endian payload_length | payload`.

## Invariants

- The four-byte length prefix is unsigned and encoded in network byte order.
- The configured maximum applies to payload bytes, excluding the prefix.
- Internal buffered bytes never exceed the configured buffer limit; the limit
  must be large enough to hold one maximum-size frame.
- One `push()` emits no more than the configured frame count or frame-byte
  processing budget. Complete buffered work beyond either budget is retained
  and reported through `BudgetExhausted` plus `needsContinuation`.
- Empty payloads are valid frames.
- Partial prefixes and partial payloads remain buffered until complete.
- One input may produce zero, one, or multiple frames up to the per-push budget.
- A declared length above the configured maximum fails closed: no frame from
  that input is emitted and the framer stays faulted until `reset()`.
- The component does not parse message ids, request ids, players, sessions, or
  serialized objects.

## Threading And Ownership

- A `PacketFramer` is owned and mutated by one caller, normally one connection's
  owner loop. It performs no internal synchronization and is not cross-thread
  safe.
- Completed payloads are returned as values; no callback is invoked and no
  re-entry path exists.
- When `needsContinuation` is true, the owner schedules `push({})` through its
  own EventLoop. PacketFramer does not know or own an EventLoop.
- Moving returned values across threads is allowed. Moving or concurrently
  invoking the framer itself is not an approved cross-thread operation.

## Failure Contract

- `FrameTooLarge` identifies the input that first declares an excessive length.
- `BufferLimitExceeded` rejects an input before append if it would exceed the
  configured buffered-byte hard limit; the framer then stays faulted.
- `BudgetExhausted` is not a protocol fault. It returns already completed frames
  and requires owner-loop continuation until no complete buffered frame remains.
- Further input returns `Faulted` and is ignored until explicit `reset()`.
- Encoding rejects payloads above the configured maximum without truncation.

## Verification

- `tests/contract/protocol/test_packet_framer_contract.cpp` covers partial
  input, sticky frames, empty frames, invalid lengths, reset, encoding, and
  segmented input.
- `tests/contract/protocol/test_packet_framer_budget.cpp` covers frame/byte
  budgets, continuation, buffer overflow, zero-length floods, and option
  validation.
- `tests/contract/protocol/test_packet_framer_round_trip_smoke.cpp` exercises
  deterministic arbitrary payload bytes inside valid framed streams, across
  varied chunk boundaries.
- `tests/fuzz/protocol/fuzz_packet_framer.cpp` is the optional
  `LLVMFuzzerTestOneInput` entry for arbitrary bytes, bounded continuation,
  reset recovery, and differential one-shot/chunked valid-stream decoding.

## Migration Provenance

- Source baseline: `mini_trantor@3eba368475a68f677aae920d4f299b155db23d57`.
- Kept invariants: unsigned big-endian length prefix, segmented input, maximum
  payload, sticky fault state, caller ownership, and transport neutrality.
- Deferred from the source design: message ids, codecs, business fields, and
  transport-specific continuation policy stay outside this primitive.
- Dropped behavior: none; the source fuzz entry and per-batch fairness are
  restored as explicit Phase 4 contracts while retaining the deterministic
  round-trip smoke as separate evidence. Ring-buffered unread storage avoids
  continuation-time whole-backlog moves under zero-frame floods.
