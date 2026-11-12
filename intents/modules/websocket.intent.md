---
status: active
target: GameNet::websocket
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# WS-C1: Bounded WebSocket server protocol

## Authorization and scope

The 2026-11-12 user-authorized WS-C1 milestone replaces the historical deferred
HTTP/TcpServer-enableSsl proposal. Native provisional GameNet::websocket is
default OFF behind GAMENET_ENABLE_WEBSOCKET, separately exported and discovered
only for COMPONENTS websocket. It depends on Core, not HTTP, RPC or OpenSSL.
OFF installs no WebSocket headers/export; Core-only consumers stay independent.
WebSocketCodec and WebSocketSession compose existing TcpServer; a runnable echo
server is the minimum server. General HTTP, client API, extensions/compression,
subprotocol negotiation stay outside WS-C1. WS-C2 separately authorizes optional
WSS composition; no HP0/release gate changes.

## WS-C2 transport seam

WebSocketSession owns a narrow WebSocketTransport, whose bundled TCP adapter
weakly observes TCP and borrows its outliving owner. Existing create(TCP) and
ws usage stay available. createWithTransport supports owned-copy admission,
readiness, graceful shutdown, explicit abort and non-reentrant abandonment.
All observations except ownerLoop are owner-only. The interface owns no socket,
thread or fallback queue and imports no TLS/OpenSSL. Not-ready start does not
arm an Upgrade deadline, take input or call user policy/ready. Transport send
and graceful shutdown must not deliver receive inline; they may report terminal
failure synchronously.
After send returns, control flushing must revalidate state before accessing
the queue because a terminal callback may already have cleared it.
onDisconnect obtains the transport's structured clean-close result, rather
than treating an encrypted TCP EOF as successful WS/TLS close.

## Wire and bounds

WS-C3 (2026-11-12) adds non-installed session-state fuzz and real WS/WSS pressure
regression without new public API or dependencies. A shared bounded sequence
runner uses the real owner loop and a test transport controlling only readiness,
send admission and terminal reentry. Every input creates fresh state, has finite
actions/bytes/receipts, uses long deadlines and resets all generation state.
Existing real-time deadline tests remain authoritative; fuzz does not sleep.
Fixed seeds run in the registered session contract; arbitrary byte actions use
tests/fuzz/websocket/fuzz_websocket_session.cpp with exact bounded evidence in
`tests/ci/run_websocket_session_fuzz.py` and `tests/ci/test_websocket_session_fuzz.py`.

The independent ledger checks rejected-no-copy, accepted transport bytes equal
drained + discarded + pending, control byte conservation and all configured caps
after every action. Callback counts/identity are generation-specific; ready and
closed occur at most once and no message/pong follows terminal. It covers split
Upgrade/frames, fragments/control, input/output overflow, close/down/abort,
abandonment, queue refusal, nested input, callback exceptions/release and terminal
reentry from transport send. Terminal zeros input/fragment/control, cancels timers
and accepted continuations become harmless; weak generations expire after drain.

tests/integration/websocket/test_websocket_tcp.cpp additionally executes repeated
real TCP pressure/recovery/probe generations. Each forces actual bounded send
refusal before reading resumes, verifies every recovered payload, alternates
graceful close with pending-output disconnect, checks zero TCP/WS residues and
replays closed prior-generation handles/callbacks while the replacement is live.
Saved old callbacks cannot mutate the new generation's counters or application.

RFC 6455 opening handshake uses strict HTTP/1.1 GET with an origin-form target,
one Host/key/version, Upgrade/Connection token lists and canonical base64 for a
16-byte nonce. Header bytes/field count are bounded. Reject bare LF, obs-fold,
whitespace before colon, CTLs, duplicate singleton headers, Transfer-Encoding,
Expect and nonzero/duplicate Content-Length. Version mismatch returns 426;
malformed/policy/oversized requests return bounded 400/403/431 with no body.
Only a successful 101 TCP admission permits ready/application frames. The
optional owner handshake policy sees owned target/host/origin; no Origin
authentication claim is made. Unrequested extensions/subprotocols are declined.

Server receives masked frames, emits unmasked canonical frames, rejects RSV,
unknown opcode, nonminimal/overflowing lengths, invalid control FIN/length and
invalid continuation ordering. Fragmented text/binary messages are reassembled
under a separate message limit. Ping/Pong/Close may interleave fragments. Text
and close reasons require strict UTF-8 (no overlong/surrogate/out-of-range code
points); close payload/code rules are checked. Errors map to 1002/1007/1009.
Complete valid events preceding a malformed frame may be delivered in wire order.
Codec faults are sticky until reset. Empty input may drain a bounded continuation.

Each input call copies/consumes only bounded bytes before callbacks. Codec input,
fragment assembly, header and control-output count/bytes have explicit limits.
Application send copies only through existing TCP admission and has no retry
queue. Rejection owns no bytes. Mandatory upgrade/pong/close output uses a finite
control queue and one weak retry timer. A close replaces pending pongs; dropped
controls are explicitly accounted. Enqueued control bytes equal admitted plus
discarded plus pending. Control saturation fails explicitly, never allocates a fallback queue.

## Five module gates

1. One connection EventLoop owns session mutation, snapshots, timers, codec and
   destruction. Foreign start/send/close/abort reject WrongThread before taking a
   self lease. Applications marshal owned values via the existing bounded owner
   executor. Stateless handshake/encoding and separately owned codecs need no loop.
2. Caller/TCP context owns the session; it weakly observes TCP and borrows its
   outliving loop. Timer closures are weak. Public dispatch and one accepted
   continuation lease session/owned input across callback release. No strong cycle.
3. Handshake policy, ready, message, pong and terminal callbacks are owner-only;
   they may send, close, abort and release caller references. Recursive input is
   rejected without consuming nested input. Callback exceptions are contained.
   Cleanup and Closed state precede exactly one terminal callback.
4. TCP-down seals sends immediately. Already received complete frames retire under
   the normal budget/one continuation before terminal settlement, even after TCP
   context release. Scheduling rejection is terminal; no inline unbounded drain.
   Partial EOF is truncated. Close/abort precedes owner shutdown.
5. Specific verification files below cover public bounds, lifecycle and failure
   behavior on Windows IOCP/Linux epoll, sanitizers, independent peers and fuzz.

## Close and failure

Created -> Handshaking -> Upgrading -> Open -> Closing -> Closed is explicit;
HTTP rejection uses Rejecting. Handshake and close have finite positive deadlines.
Local close seals application admission, drains an accepted close frame and waits
for peer Close; both admitted/sent and received Close trigger TCP shutdown. Actual
TCP-down is graceful only after both Close frames and no pending controls.
Protocol failure may send its close then shut down TCP without parsing more bad
input. Preserve the original failure reason if close times out. Missing peer Close
is Truncated; abort, timer/post rejection and transport failure are explicit.
Repeated close/down/abort cannot duplicate terminal callbacks. Terminal settlement
clears header/codec/fragment/control storage and timers and reconciles all control
bytes. Destruction settles Aborted without reentrant TCP force-close from context
deleters. Caller explicitly aborts when immediate forced teardown is needed.

## Verification

- tests/unit/websocket/test_websocket_codec.cpp: RFC handshake vector, strict HTTP,
  masking/lengths, arbitrary segmentation, fragments/control interleaving, UTF-8,
  close codes, limits, reset and bounded continuations.
- tests/contract/websocket/test_websocket_session.cpp: real connected TCP sockets,
  handshake/close deadlines, owner/re-entry/release, bounded output recovery,
  protocol/transport failures, EOF continuation and zero control/input residue.
- tests/integration/websocket/test_websocket_tcp.cpp: real loopback TcpServer and
  independent masked peer, fragmented upgrade/message, ping, close and reconnect.
- tests/fuzz/websocket/fuzz_websocket.cpp: bounded malformed handshake/frame bytes,
  segmentation equivalence and canonical server-frame encoding oracle.
- tests/ci/run_websocket_contracts.py: exact configured/executed inventories,
  real external websockets 15.0.1 interoperability, installed ON/OFF consumers,
  sanitizer and bounded fuzz evidence with exact source/run identity.
- tests/ci/test_websocket_workflow.py: missing/skipped/count/seed evidence rejection
  and default-OFF package/workflow boundaries.

Functional development evidence does not establish production adoption/capacity.
