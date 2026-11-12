---
status: active
target: GameNet::rpc
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# Module Intent: Minimal Callback RPC (RPC-C1)

## Authorization and scope

The user-authorized 2026-11-11 RPC-C1 milestone in plan.md replaces the historical
deferred callback/coroutine proposal with a native provisional GameNet::rpc.
It does not promote either audited consumer wire, alter the stable Reactor,
or satisfy HP0/HP7 performance gates. RpcCodec and one bidirectional RpcChannel
compose existing TcpServer/TcpClient; applications wire message/down callbacks.
The channel never replaces Core callbacks, owns a socket or starts a thread.
RPC-C2 separately authorizes the optional TLS transport composition described in
rpc_tls.intent.md. Coroutines, discovery, retries, authentication and streaming stay deferred.
Exactly-once means local completion, not exactly-once remote side effects.

## Wire and admission

PacketFramer supplies a four-byte big-endian body length. The body is
GNRP | version:u8=1 | kind:u8 | reserved:u16=0 | id:u64 | method:u32 |
remainingTimeoutMs:u32 | payload. Integers are big-endian. Kinds are request=1,
response=2, error=3. Request IDs are nonzero, strictly increasing per direction
and never reused or wrapped. Request method and timeout are positive;
response/error method and timeout are zero. Payload is opaque bytes, default
64 KiB maximum. This is not the historical gateway/YanGame wire. Local
steady_clock deadlines use relative wire timeouts, never foreign clocks.

Positive count and charged-wire-byte limits independently bound outgoing
pending and incoming deferred replies. Framing has buffered-byte, frame-count
and per-turn byte limits. Rejected call admission returns a typed reason and
id=0, invokes no callback and retains no timer/byte slot. Accepted calls own
one callback/deadline until one terminal result. trySend failure never falls
back to an unbounded queue. Admission is transactional even if synchronous
send failure re-enters the disconnect hook.

## Owner, ownership and re-entry

1. One connection EventLoop owns mutation, handlers, callbacks, cancellation,
   snapshots and destruction. Foreign call/cancel/reply/close return WrongThread.
   Applications marshal owned values through bounded EventLoopExecutor posts
   and check PostResult. No new cross-thread scheduling mechanism is added.
2. Caller or connection context owns the channel; it weakly observes the
   connection and borrows the outliving EventLoop. Timer and saved reply
   closures are weak; accepted continuations temporarily lease the channel.
3. onMessage copies into bounded PacketFramer and consumes the supplied Buffer
   before handlers. Frames own their payload. No span survives a continuation.
   Recursive onMessage is rejected without consuming the nested Buffer.
4. Callbacks may call/cancel/reply/close and release caller references. Pending
   removal, timer cancellation and byte release precede callback invocation.
   Public dispatch holds a self lease. Callback exceptions are contained/counted.
5. Applications call onDisconnect from the down callback and close before
   quitting/destroying the loop. Owner-thread destruction also settles remaining
   outgoing callbacks as Shutdown. A reply after channel destruction is Closed.

## RPC-C2 transport seam

RpcTransport supplies only an outliving owner loop, owner-only NotReady/Ready/
Closed observation, bounded copied-byte send admission and abort. RpcChannel
owns that transport; bundled TCP and TLS implementations weakly observe their
connection/stream. create(TcpConnection) keeps its existing behavior and uses
the same narrow seam as createWithTransport. RPC never discovers OpenSSL.

NotReady rejects call before reserving an ID, timer, callback or pending bytes.
Send rejection rolls back provisional admission even when a synchronous
transport failure re-enters onDisconnect. Accepted transport bytes are not a
remote execution acknowledgement. Snapshot dispatch/continuation observations
permit a TLS composition to retire already-received RPC frames before closing;
the existing TCP onDisconnect remains immediate and source-compatible.

## Terminal contract

Response, remote error, deadline, cancel, disconnect, explicit shutdown,
protocol error and continuation rejection converge on one removal path. Only
admitted outgoing entries invoke callbacks, exactly once. Late/duplicate replies
cannot complete removed entries. Response processing checks the local deadline
even if TimerQueue delivery is delayed. Local cancel cannot undo remote work.

Inbound admission reserves count/bytes/deadline. First reply removes the slot
before send; later replies return NotFound. Deadline, close, handler exception
and overload have explicit settlement. Absent handlers return a bounded error;
application handlers dispatch method IDs. Malformed wire closes channel and TCP.
Buffered frames drain via one bounded continuation; scheduling rejection closes
and settles all obligations. Explicit close seals admission before callbacks.

## Verification

RPC-C3 (2026-11-12) adds validation without changing wire/admission contracts.
`tests/support/RpcStateSequence.h` owns a bounded independent per-call ledger on
one real EventLoop. After each action, admitted minus completed must equal
pending count and charged bytes; rejected calls never invoke callbacks. It
mixes transport refusal/readiness, synchronous disconnect, response/error and
duplicates, fragmented/malformed input, deferred replies, callback re-entry,
close and channel replacement. Final close plus accepted continuation retirement
requires zero outgoing/inbound count/bytes and expired weak channel/reply handles.
Long deadlines isolate byte-driven fuzz from wall-clock scheduling; existing
real deadline contracts remain mandatory. No test-only runtime clock/API is added.

- tests/fuzz/rpc/fuzz_rpc_codec.cpp: independent wire validity oracle, arbitrary
  malformed bytes/limits and canonical decode/encode round trips.
- tests/fuzz/rpc/fuzz_rpc_channel.cpp: bounded byte-driven state sequence using
  the same harness as the ordinary contract; no real peer or sleeping per input.
- tests/support/RpcReconnectStress.h: real TCP burst rejection, read recovery,
  successful probe, alternating cancellation/pending disconnect and repeated
  generations, independent callback accounting and zero-residue teardown.
- tests/ci/run_rpc_fuzz.py: exact-run bounded fuzz, replayable corpus, sanitizer
  diagnostics and clean checkout/run identity retained as development evidence.

- tests/unit/rpc/test_rpc_codec.cpp: wire, binary/partial frames, malformed/limits.
- tests/contract/rpc/test_rpc_channel.cpp: connected-socket terminal races, bounded pending,
  re-entry, exceptions, deferred replies and shutdown on epoll and IOCP.
- tests/integration/rpc/test_rpc_tcp.cpp: real TCP client/server request/response,
  timeout/cancel/late reply, pending overload/recovery, pending disconnect and
  cross-thread bounded owner submission.
- tests/cmake/test_install_package_contract.py: installed GameNet::rpc linkage
  through the provisional package consumer.

## Review

Review wire/terminal contract, bounds, owner/weak lifetime and lifecycle before
implementation details. Functional development evidence does not imply external
adoption, performance promotion or release. The five core change-gate answers
are in the owner/ownership/re-entry section and named verification paths above.
