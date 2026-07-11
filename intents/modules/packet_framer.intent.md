---
status: active
target: GameNet::protocol
migration_source: native
promote_gate: none
---

# Module Intent: PacketFramer

## Intent

`GameNet::protocol` needs one transport-neutral primitive that converts an
arbitrarily segmented byte stream into length-delimited payloads. The first
format is exactly `uint32 big-endian payload_length | payload`.

## Invariants

- The four-byte length prefix is unsigned and encoded in network byte order.
- The configured maximum applies to payload bytes, excluding the prefix.
- Empty payloads are valid frames.
- Partial prefixes and partial payloads remain buffered until complete.
- One input may produce zero, one, or multiple frames.
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
- Moving returned values across threads is allowed. Moving or concurrently
  invoking the framer itself is not an approved cross-thread operation.

## Failure Contract

- `FrameTooLarge` identifies the input that first declares an excessive length.
- Further input returns `Faulted` and is ignored until explicit `reset()`.
- Encoding rejects payloads above the configured maximum without truncation.

## Verification

- `tests/contract/protocol/test_packet_framer_contract.cpp` covers partial
  input, sticky frames, empty frames, invalid lengths, reset, encoding, and
  segmented input.
- `tests/contract/protocol/test_packet_framer_fuzz_smoke.cpp` exercises
  deterministic arbitrary byte streams and chunk boundaries.
