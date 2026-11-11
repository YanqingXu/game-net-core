---
status: active
target: GameNet::rpc_tls
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# RPC-C2: Optional authenticated RPC over TLS

## Scope and authorization

The 2026-11-11 user-authorized RPC-C2 milestone in plan.md adds provisional
GameNet::rpc_tls, built only with GAMENET_ENABLE_TLS. Its separate export loads
only for COMPONENTS rpc_tls. Core/RPC-only consumers require no OpenSSL, even
from an ON installation. OFF installs no rpc_tls surface. Existing TCP factories,
wire, callbacks and terminal behavior remain available. This is explicit
composition, not STARTTLS, mTLS, retry, streaming or ConnectionTransport promotion.

## Five module gates

1. One connection EventLoop owns session/channel/SSL state, callbacks, snapshots,
   close and destruction. Foreign mutations reject before acquiring a self lease;
   cross-thread callers marshal owned values via the existing bounded executor.
2. Caller/context owns RpcTlsSession and its TlsStream. TLS callback bridge owns
   RpcChannel and user callbacks and borrows its outliving owner. RPC owns a
   focused RpcTransport that weakly observes TLS. There is no
   strong ownership cycle. Accepted TLS EOF and RPC settlement continuations
   retain only their owning stream/bridge until already received input settles,
   even if the TCP-down hook clears the application context. Application callback
   state lives until session closed; explicit shutdown precedes owner teardown.
3. Ready, RPC handlers/completions and closed are owner-only and may call, cancel,
   close, abort and release caller references. Public dispatch leases the session;
   bridge/TLS/queued frames retain their own data. Callback exceptions cannot
   prevent terminal settlement. Cleanup precedes the single closed callback.
4. Existing RPC pending count/bytes, parser budget, TLS plaintext count/bytes,
   cipher bounds and TCP output budgets compose. No new unbounded send/input
   queue, foreign-thread inline fallback or recursive RPC/TLS dispatch is added.
5. Specific tests below enforce admission, ownership, re-entry and zero-residue
   behavior on real OpenSSL and both production TCP backends.

## Admission and failure

Create/start on TCP-up; only authenticated TLS-ready admits RPC calls. Created
or handshaking calls return NotReady/id=0 without taking a callback, timer or
pending reservation. After TLS output closes, calls return Closed. Every RPC
frame enters TlsStream::trySend; raw TcpConnection send is never a TLS fallback.
Rejected transport send creates no RPC callback obligation. Accepted calls
terminate once through response, timeout, cancel, shutdown or disconnect.

Client trust/name/expiry validation remains TlsContext policy, with no bypass.
Certificate rejection and handshake timeout cannot report ready, accept a call
or leave pending state. Preserve TLS terminal diagnostics separately from the
RPC completion status. Cancellation is local; no exactly-once remote execution
or automatic retry guarantee is added. TLS write admission/cipher drain remain
the existing TLS contract, not acknowledgement of an RPC response.

## Terminal ordering

TLS closed seals RPC transport admission immediately. Already authenticated
plaintext may still be in a bounded RpcChannel continuation: defer session
settlement through at most one accepted owner post until dispatch/continuation
retires. Then onDisconnect settles remaining calls as Disconnected, clears
inbound reservations and timers, and closed fires once. A sealed transport does
not admit new incoming handlers: authenticated queued responses can complete
existing calls while requests are counted rejected. Rejected settlement post
closes RPC without executing buffered handlers inline and reports SchedulingRejected.
Repeated down/abort/close never duplicates completions or the terminal event.

Explicit shutdown/abort seals RPC first and settles pending as Shutdown, then
drains/aborts TLS. Destroying an active session settles Shutdown/Aborted using
the existing non-reentrant TLS abandonment policy. After actual TCP-down,
clearing the context does not abandon pending TLS EOF work: the acyclic bridge
and accepted leases retain callbacks until settlement. The owner must outlive
all accepted work. Closed callback exceptions are contained.

## Verification

- tests/contract/rpc/test_rpc_channel.cpp: TCP compatibility and transport seam,
  NotReady, send rollback/re-entry, bounded pending and local terminal semantics.
- tests/contract/rpc_tls/test_rpc_tls_session.cpp: real OpenSSL handshake gate,
  CA/name/expiry/timeout rejection, request timeout/cancel, pending/TLS/TCP
  backpressure, disconnect, re-entry, abandonment and exactly-once settlement.
- tests/integration/rpc_tls/test_rpc_tls_tcp.cpp: real TcpServer/TcpClient RPC/TLS,
  binary responses and independent OpenSSL peer batching frames then closing,
  owner-budgeted EOF drain and context release on Windows IOCP/Linux epoll.
- tests/ci/test_tls_workflow.py: exact optional inventory/execution and separately
  installed rpc_tls consumers; Core/RPC-only and OFF dependency isolation.

Windows/Linux Release and Linux ASan/UBSan/TSan plus Windows ASan must execute
real TLS contracts without skip/fallback. Existing default RPC/TCP tests remain
in ordinary CI. Development validation does not promote HP performance or publish.
