---
status: active
target: GameNet::tls
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# TLS-C1: Optional OpenSSL stream over TCP

## Authorization and scope

The 2026-11-11 user-authorized TLS-C1 feature milestone in plan.md replaces the
historical deferred TLS-in-TcpConnection proposal. GameNet::tls is provisional,
default OFF (GAMENET_ENABLE_TLS), separately exported and dependency-discovered
only when requested. OFF requires no OpenSSL and installs no TLS surface.
This native component does not promote M9 consumer-private TLS or HP0/HP7.
It supports OpenSSL 3, TLS 1.2/1.3 and one TlsStream per existing TCP connection.
It does not activate the deferred ConnectionTransport design, add STARTTLS,
coroutines, WebSocket, automatic RPC/TLS routing, credential reload or mTLS.

## Architecture

An immutable shared TlsContext owns SSL_CTX. TlsStream owns SSL and a bounded
BIO pair; the existing TcpConnection carries ciphertext on epoll/IOCP. TLS
never reads/writes the socket directly and never modifies Core callback slots.
Applications configure TCP callbacks before start, create/start TLS on TCP-up,
and forward message, write-complete and TCP-down to the stream. TCP-up is not
TLS-ready. Only the TLS ready callback permits application plaintext.

## Invariants and ownership (five module gates)

1. The connection EventLoop owns stream state, all SSL/BIO operations, callbacks,
   snapshots, close and destruction. Context factories configure SSL_CTX before
   publication; later context use is read-only. Foreign mutations return
   WrongThread before taking ownership. Cross-thread work marshals owned values
   through the existing bounded owner executor and checks admission.
2. Caller/context owns TlsStream. It weakly observes TcpConnection and borrows
   its outliving EventLoop. It keeps its shared TlsContext until SSL is freed.
   Timer closures are weak; public dispatch leases self across reentrant user
   callbacks. At TCP EOF a bounded accepted owner continuation leases already
   received cipher input until TLS settlement, even after context release.
   TLS terminal (not raw TCP-down) ends application callback lifetime: callbacks
   retain their application/session state. SSL owns its internal BIO; the stream
   owns the network BIO.
3. Ready/data/terminal callbacks run on the owner, may send, pause/resume, shut
   down, abort or release caller references. No nested SSL dispatch occurs.
   Terminal state/budget/timer cleanup precedes the single terminal callback.
   Callback exceptions are contained and fail the stream; callback payload is
   borrowed only for that invocation. Recursive onMessage rejects nested input.
4. Cipher input is copied/consumed before user dispatch. BIO capacities,
   encrypted-input queue, pending plaintext bytes/count and SSL work per turn
   are bounded. WANT_READ/WANT_WRITE are retry states, not terminal errors.
   Failed writes retry the same bytes/length before advancing their queue.
   TCP send rejection leaves BIO bytes intact; one bounded timer retries, even
   if TCP's optional write notification was dropped. No busy spin/unbounded post.
5. Specific verification files are named below. Applications close/abort streams
   before owner shutdown, and always forward TCP-down. No timer keeps the owner
   alive. Owner destruction cancels timers, releases buffers and reports Aborted
   if terminal settlement was still outstanding.

TCP EOF first drains previously accepted cipher records under the normal work
budget, then classifies close_notify/truncation. Rejected EOF continuation is
SchedulingRejected, never a silent clean close. Destruction does not force-close
TCP reentrantly through a context deleter: abandonment reports Aborted and seals
TCP output by shutdown; use explicit abort for immediate forced teardown.

## Authentication and failure

Client contexts always verify the chain using explicit CA PEM or default trust
paths. Each client stream requires a nonempty expected DNS/IP identity; SAN and
full-wildcard rules apply and DNS sends SNI. There is no insecure-skip-verify API.
Server contexts load a certificate chain/private key and reject mismatch.
Minimum TLS 1.2, no compression, no renegotiation, no early application data.
Handshake and shutdown have finite positive deadlines. Invalid certificate,
name, expiry, handshake/protocol errors, input overflow, timer rejection,
callback exception and TCP EOF without close_notify have explicit terminal
results. Errors never become a successful handshake or silent clean EOF.

## Backpressure and close

trySend only admits in Open, with count/byte caps, copying before Accepted.
Rejected admission changes no ownership. Accepted plaintext remains queued until
SSL consumes it, or is counted discarded on terminal failure. Ciphertext goes
through existing multi-level TCP output admission; bounded BIO bytes survive
Overloaded and resume on write-complete/timed retry.

pauseRead stops application plaintext delivery; resumeRead processes buffered
records. It is TLS-level backpressure, not a new Core socket-read switch.
Ciphertext continues to be bounded; sustained traffic beyond the configured
input cap closes with InputLimit rather than allocating indefinitely.

shutdown seals plaintext admission immediately, drains accepted plaintext,
sends close_notify, receives the peer close_notify, and submits TCP shutdown
only after TLS output was admitted. TCP-down is Graceful only after both TLS
alerts and zero TLS pending output; unexpected EOF is Truncated. A stalled
peer or paused reader is bounded by shutdown timeout (resume to complete a
paused close). Abort/timeout/fatal error use normal TCP force-close cleanup.
Close during handshake is Aborted. Repeated close/down never duplicates terminal
callbacks. TLS-ready and terminal events remain separate from Core's TCP events.

## Verification

- tests/unit/tls/test_tls_context.cpp: real certificates, trust/key validation.
- tests/contract/tls/test_tls_stream.cpp: fragmented handshake, verification
  failures, WANT progress, queue/input caps, pause/resume, re-entry, deadlines,
  dropped write notifications, close_notify, truncation and terminal settlement.
- tests/integration/tls/test_tls_tcp.cpp: real TCP/TLS echo, backpressure and
  graceful shutdown on Windows IOCP/Linux epoll, plus OpenSSL interoperability.
- tests/ci/test_tls_workflow.py: default-OFF package isolation, ON-only component
  and registered cross-platform TLS evidence lanes.

The TLS workflow runs the exact optional inventory and installed consumers.
Default Core CI remains the 133-test baseline. TLS-enabled adds three tests;
no sanitizer or real-OpenSSL scenario may silently fall back/skip as evidence.
