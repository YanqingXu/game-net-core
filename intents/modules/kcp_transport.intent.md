---
status: active
target: GameNet::kcp
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# Module Intent: KCP-C1 bounded standard KCP

## Authorization and boundary

The user authorized KCP-C1 on 2026-11-13. This replaces the deferred custom
KCP-style preview with standard message-mode KCP, pinned to upstream
skywind3000/kcp commit b1a7a2101dcbb96017681a500d6b82bbe5a88766 (MIT).
Vendored source is unchanged, hash-checked and privately symbol-prefixed.
GAMENET_ENABLE_KCP defaults OFF and requires explicit GAMENET_ENABLE_UDP=ON.
GameNet::kcp composes only Core and UDP; Core/UDP consumers acquire neither KCP
nor OpenSSL. Existing Core, UDP and TransportEndpoint APIs remain unchanged.

Provide explicit reliable ordered message sessions over one owner-loop UDP
server: conservative fixed MTU, fragmentation/reassembly, duplicate suppression,
retransmission and upstream default congestion control. No custom wire envelope,
handshake, encryption, authentication, FEC, PMTU discovery, auto-created peers or
TransportEndpoint adapter belongs to C1. A local protocol ACK is not evidence
that the remote application processed a message.

## Identity, ownership and threads

One EventLoop owns creation/start/open/send, engine updates, receive and all
callbacks. Sessions are explicitly opened against a known address and nonzero
conv; each conv can be used only once in a transport's bounded lifetime history.
History exhaustion rejects new sessions. The application must select fresh conv
values across transport/process restarts; this is isolation, not authentication.
UDP peer identity/generation prevents old handles and late local completions
from affecting replacements. Wrong-conv datagrams do not mutate a new engine.

The wrapper owns shared state; active state holds a lease until terminal cleanup.
Handles, timer/post closures and UDP callback bridges weakly observe it.
requestClose, stop and destruction synchronously seal admission under a gate
and signal a pre-attached lifecycle source; they are thread-safe. Data/update
work uses the bounded normal executor, never the lifecycle lane. Timers and
coalesced continuations visit bounded sessions/messages per turn.

No user callback runs inside upstream KCP or under a gate lock. Callbacks may
reenter, throw, close/stop or release the last external reference. Acknowledgment
dispositions commit before deferred user callbacks and before later close seals.
Every accepted send terminates exactly once locally as Acknowledged, Cancelled
or Failed; rejected sends have id zero, no reservation and no completion.
Pending reservations are released before callbacks. First close wins. closed
waits for protocol/pending cleanup and the original UDP peer's native retirement;
stopped waits for all records, timers, posts and native IOCP completions.

## Bounded algorithm and failure behavior

Validate all options before allocating. Bound sessions and conv history,
per-session/global pending message count and bytes (including empty messages),
send segments, receive window/reorder slots, ACK entries, protocol storage,
UDP output count/bytes, datagram/segment work and callback work per turn.
Reserve the full fixed per-session arena and receive scratch against a global
protocol-memory budget before creating the engine; no unbounded upstream malloc.

Compile the unchanged private upstream copy as C++, installing private allocator
hooks once. A scoped thread-local arena supplies fixed small/large blocks.
Preallocate ACK storage: upstream otherwise grows it independently of windows
and aborts on allocation failure. Exhaustion throws through this C++ copy;
catch at the engine boundary and discard the entire arena without traversing a
potentially partially mutated upstream list. No exception escapes the adapter.

Preflight an entire datagram before input: standard header/commands, lengths,
conv, fragmentation bounds and ACK capacity. Invalid trailing segments cannot
partially acknowledge earlier ones. ACK RTT samples older than the upstream
maximum RTO (60 seconds) are rejected before its signed RTT arithmetic.
ACK-budget pressure drops the whole packet
for retransmission. Reserve space for all send fragments before ikcp_send.
UDP output refusal is modeled as wire loss, with no secondary output queue;
bounded retransmission and send lifetime eventually fail an unreachable session.
Receive messages exceeding the configured application limit fail explicitly.
Scheduling refusal seals and cleans up through the lifecycle source. Close is
local cancellation, without a network close handshake or remote delivery promise.

## Contracts and evidence

- tests/unit/kcp/test_kcp_engine.cpp: standard framing, whole-packet validation,
  fixed arena/ACK/fragment bounds and deterministic loss/reorder/duplicate recovery.
- tests/contract/kcp/test_kcp_transport.cpp: options, admission, cross-thread seal,
  reentry/release, timeout, exactly-once completion, generation and zero residue.
- tests/integration/kcp/test_kcp_reliable.cpp: real IPv4/IPv6 UDP, burst/backpressure
  recovery, fairness and multiple fresh-conv generations.
- tests/fuzz/kcp/fuzz_kcp_protocol.cpp and tests/fuzz/kcp/fuzz_kcp_session.cpp:
  bounded malformed input and state-sequence fuzz with settlement/budget oracles.
- tests/ci/run_kcp_contracts.py: Windows/Linux Release, Windows ASan,
  Linux ASan/UBSan and TSan, independent unchanged upstream-only UDP peer,
  loss/ACK-loss/reorder/duplicate/delay, installed ON/OFF consumer isolation.

Each fuzz target requires at least 20,000 bounded sanitizer executions and exact
head/inventory/raw-log evidence. Existing Core, UDP, TLS/RPC-TLS, WS/WSS,
WebSocket/RPC/HP1 fuzz checks remain required. This work submits a PR; it does
not authorize merging, releasing, or claims of production/performance validation.
