# Optional WSS server composition (WS-C2)

Enable both `GAMENET_ENABLE_WEBSOCKET=ON` and `GAMENET_ENABLE_TLS=ON`, then
request `find_package(GameNetCore REQUIRED COMPONENTS websocket_tls)` and link
`GameNet::websocket_tls`. This provisional component composes the existing
bounded WS server with real OpenSSL TLS. Plain `WebSocketSession::create(TCP)`
and ws usage remain available; `GameNet::websocket` depends only on Core.
Requesting Core or websocket alone from a dual-ON package never finds OpenSSL
or loads TLS exports. Either flag OFF installs no WSS header or export and
required WSS discovery fails with an explicit unavailable-component diagnostic.
The published v0.3.1 packages predate this development component.

Create a server `TlsContext` from a certificate chain and matching private key,
then create `WebSocketTlsSession` on TCP-up with a required `closed` callback.
Forward TCP ciphertext buffers to `onMessage`, write-complete to `onWritable`,
and actual TCP-down to `onDisconnect` before releasing the context. The adapter
does not replace Core callbacks or read/write the socket itself. It supports
server sessions; general HTTP, client WS API, compression, subprotocols, STARTTLS
and mTLS are separate work.

TLS-ready starts the bounded Upgrade parser and its deadline. Only accepted
101 plaintext output enables WS-ready; send before this returns NotReady. Every
Upgrade/control/data byte enters TLS, with no plaintext fallback. All state,
callbacks, snapshots and destruction share the connection EventLoop. Foreign
mutation returns WrongThread before leasing self; use bounded ownerExecutor
posts with owned values and check admission.

Application send copies only after bounded TLS admission; refusal adds no
retry queue. WS control count/bytes, TLS plaintext count/bytes, encrypted input,
BIO capacity and TCP output budgets compose. Existing weak control/TLS retry
timers recover progress even without write-complete notifications. Snapshots
retain WS control and TLS plaintext ledgers and separate failure diagnostics.

Close seals WS sends, exchanges WS Close, then drains TLS plaintext and exchanges
both close_notify alerts before TCP shutdown. TLS handshake, Upgrade, WS Close
and TLS shutdown have separate finite deadlines. `terminal.graceful()` requires
both WS and TLS Graceful. A WS Close alone cannot turn truncated TLS into a
successful shutdown; TLS close_notify alone cannot replace a missing WS Close.
Protocol/timeout reasons survive teardown.
The TLS shutdown drive uses one bounded weak owner post, preventing recursive
plaintext delivery into an active WS parser. Rejection is explicit, with no
inline fallback or extra byte queue.

Caller/context owns the wrapper and TLS. TLS callback bridge owns WS, whose
transport weakly observes TLS. Weak callbacks avoid a cycle. Raw TCP-down seals
admission first; TLS retires already received ciphertext, then WS retires
decrypted frames under its normal per-turn budget. One accepted bridge post
retains callback state until WS continuations finish, including context release.
Rejected posting reports SchedulingRejected and clears outstanding logical work.
After both cleanups, `closed` runs exactly once with zero logical WS input,
fragment/control and TLS plaintext/cipher/BIO residue and reconciled ledgers.
Callbacks may close/abort/release references; exceptions are contained. Explicit
abort forces transport teardown. Active wrapper abandonment uses non-reentrant
WS/TLS cleanup; raw-EOF leases finish independently. Close/abort before owner exit.

`gamenet_websocket_tls_echo_server` is a non-installed integration peer built
with testing and both options ON. It accepts certificate/key, optional loopback
port and bounded lifetime arguments; port zero prints its actual bound port.
Its `/echo` policy and finite output cap support reproducible external peers.

The independent pinned `websockets==15.0.1` sync client runs eight generations,
16 fragmented Unicode/binary exchanges and eight Ping/Pong checks with strict
CA/hostname validation. Its raw socket teardown omits TLS close_notify: each
WS Close(1000) still produces an explicit truncated WSS terminal. An additional
independent Python stdlib TLS peer performs masked WS Close plus `unwrap()` and
must produce one fully graceful layered terminal. Unknown CA, wrong hostname,
expired certificate and HTTP policy refusal are mandatory negative cases.
The test certificate issuer supplies SKI/AKI for strict X.509 verification;
there is no insecure verification bypass.

`.github/workflows/websocket-tls.yml` runs Windows/Linux Release (143 tests),
Linux ASan/UBSan, Linux TSan and Windows ASan (ten WS/TLS/RPC-TLS/WSS tests).
Every lane runs real independent WSS peers and installed Core/WS/WSS consumers.
Release also builds and installs WS-only, TLS-only and both-OFF packages.
Exact JUnit/inventories, source/run identity, commands, logs and file hashes are
retained. Existing plain WS, RPC and HP1 fuzz remain independent regression gates.
These finite correctness runs do not establish production capacity or endurance.
