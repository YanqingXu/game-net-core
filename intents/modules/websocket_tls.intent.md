---
status: active
target: GameNet::websocket_tls
migration_source: native
promote_gate: none
artifact_kind: installed-library
migration_mode: native
source_commit: none
source_paths: none
---

# WS-C2: Optional server WebSocket over TLS

## Authorization and bounds

The 2026-11-12 user-authorized WS-C2 milestone adds provisional
GameNet::websocket_tls only when GAMENET_ENABLE_WEBSOCKET and GAMENET_ENABLE_TLS
are ON. Its separate export loads only for COMPONENTS websocket_tls. Requesting
Core or websocket alone never finds OpenSSL, including from a dual-ON install.
Either OFF installs no WSS header/export. Existing ws/TCP API remains usable.
No general HTTP/client WS, compression, subprotocol, mTLS, STARTTLS or automatic
plaintext fallback. The explicit server adapter requires a server TlsContext.

## Five module gates

1. One connection EventLoop owns WS/TLS/session/bridge state, all callbacks,
   snapshots and destruction. Foreign mutation rejects before acquiring a lease;
   use existing bounded ownerExecutor with owned input. No new executor/thread.
2. Caller/TCP context owns WebSocketTlsSession and TlsStream; TLS callbacks own
   a bridge which owns WebSocketSession. Its transport weakly observes TLS. All
   borrow the outliving loop, timers are weak, dispatch and accepted EOF/WS
   continuations lease only owned state. No strong cycle. TLS callback state
   outlives raw context release while already received ciphertext is retired.
3. Policy/ready/message/pong/closed run on owner and may send/close/abort/release.
   WS consumes copied plaintext before dispatch; recursive input is rejected.
   Exceptions are contained. Cleanup precedes exactly one combined closed event.
4. Existing WS input/fragment/control, TLS plaintext/cipher/BIO/work and TCP output
   caps compose. No application retry queue. WS mandatory controls retain finite
   storage until TLS admission and use the existing one weak retry timer. TCP/TLS
   rejection must recover through writable/timer progress, even with dropped
   writable notifications. Accepted plaintext is encrypted or explicitly discarded.
5. Tests named below verify true TLS, lifecycle and failure on both TCP backends.

## Admission and terminal order

TCP-up starts TLS; only TLS-ready starts the WS Upgrade deadline/parser. No policy,
application ready or frame send before TLS plus accepted 101. Every WS response,
control and application frame uses TlsStream::trySend. No raw TCP path exists.
Trusted independent clients verify CA, DNS/IP and expiry; rejection never becomes
WS-ready. TLS and WS diagnostics remain separately observable.

Local Close seals WS application sends, admits the WS Close, receives peer Close,
then requests TLS shutdown. TLS drains accepted plaintext and both close_notify
alerts before TCP shutdown. Combined Graceful requires WS Graceful and TLS
Graceful; a Close without close_notify or close_notify without Close is truncated.
TLS shutdown is submitted through one bounded weak owner post, so its SSL read
drive cannot recursively deliver plaintext into the active WS parser. Refusal
is SchedulingRejected; no inline fallback. This post owns no session or bytes.
TLS handshake, WS Upgrade, WS Close and TLS shutdown each have finite deadlines.
Preserve protocol/timeout/abort reasons instead of replacing them with teardown.

Raw TCP-down is forwarded to TLS only. TLS first retires accepted cipher input;
its terminal event seals WS sends and starts WS onDisconnect. WS retires already
decoded complete frames under its normal per-turn budget before combined closed.
One bounded bridge continuation retains callback state until existing WS work
retires. Rejected settlement posting aborts remaining WS work and reports
SchedulingRejected; no recursive unbounded drain or stranded callback state.
After terminal, input/fragment/control and TLS pending plain/cipher/BIO bytes are
zero, control and plaintext ledgers reconcile, timers are cancelled, and repeated
close/down/abort cannot duplicate closed. Rejected continuation is explicit.
Wrapper destruction with raw EOF pending lets accepted leases finish; active
abandonment seals WS and TLS without force-closing TCP inside a context deleter.
Explicit abort is allowed to force-close. Close/abort before owner destruction.

## Verification

The 2026-11-13 user-authorized close interoperability hardening preserves all
installed APIs and dependency boundaries. Bounded non-installed diagnostics
correlate each client generation with server WS/TLS/TCP ownership: sent/received
Close frames, code/reason, SSL/receive exceptions, close_notify flags, pending
bytes and unique terminal outcome. Strict Close(1000) and layered close checks
remain mandatory. Retrying an intermittent failure is not root-cause evidence.
Controllable record segmentation, Close/EOF/close_notify ordering and owner
continuations must reproduce the identified failure before its fix and pass
afterwards, with exactly one terminal and zero retained output. Existing tests
below remain the contract anchors; Windows/Linux, sanitizer and independent
multi-round WSS peers verify the final frozen source without skipped evidence.

WS-C3 extends tests/integration/websocket_tls/test_websocket_tls_tcp.cpp with
the same real loopback pressure/reconnect driver used by plain WS. It composes
real authenticated TlsStream on the independent wire client and the WSS server;
no test transport substitutes for TLS. Read holding has an explicit byte cap.
Each generation rejects output, resumes reading and verifies all accepted frames
plus a fresh probe. Normal WS Close + close_notify and abrupt pending disconnect
alternate. Combined terminal count is one; WS input/fragment/control, TLS pending
plain/cipher/BIO and TCP output clear and both ledgers reconcile. Closed old
handles/callbacks execute against old state while a new generation is live;
they cannot target the replacement. All weak session/connection references must
expire after the intentionally retained stale handle is released and owner work
retires. Independent ws/wss peers and all existing fuzz remain regression gates.

- tests/contract/websocket/test_websocket_session.cpp: TCP compatibility and
  transport readiness, reentrant terminal send and refusal rollback.
- tests/contract/websocket_tls/test_websocket_tls_session.cpp: real TLS and strict
  Upgrade gate, CA/name/expiry rejection, all deadlines, admission/recovery, close
  order, abrupt EOF, callback reentry/release, exactly-once and zero ledgers.
- tests/integration/websocket_tls/test_websocket_tls_tcp.cpp: real loopback WSS,
  fragmented/binary/control exchanges, reconnect and EOF context release.
- tests/ci/run_websocket_tls_contracts.py: exact inventories/JUnit, independent
  pinned WSS client, source/run/file identity and ON/single-OFF consumer isolation.
- tests/ci/test_websocket_tls_workflow.py: reject missing/skipped evidence and
  preserve optional dependency boundaries and sanitizer lanes.

Windows/Linux Release, Linux ASan/UBSan/TSan and Windows ASan execute real TLS
without skip/fallback. Existing WS/RPC/HP1 bounded fuzz remain regression gates.
Finite development stress does not establish HP0, production capacity or release.
