# RPC over TLS (RPC-C2)

`GameNet::rpc_tls` explicitly composes the existing callback RPC protocol and
OpenSSL TLS stream on one connection EventLoop. It is provisional and built only
with `GAMENET_ENABLE_TLS=ON`. Existing `RpcChannel::create(connection, ...)` and
TCP callbacks remain unchanged. The RPC library itself never depends on OpenSSL.

```cmake
find_package(GameNetCore REQUIRED COMPONENTS rpc_tls)
target_link_libraries(app PRIVATE GameNet::rpc_tls)
```

Requesting `rpc_tls` loads its separate export plus TLS/OpenSSL. Requesting only
Core or `rpc` does not, even from a TLS-enabled installation. Requesting only
`tls` does not load `rpc_tls`. An OFF package installs neither TLS nor RPC/TLS
headers or exports and rejects either required optional component explicitly.
The published v0.3.1 packages predate these development components.

## Composition

Configure TcpServer/TcpClient callbacks before starting TCP. The connection's
context owns the session. The following code belongs in its TCP-up callback:

```cpp
gamenet::rpc::RpcTlsOptions options;
options.peerName = "localhost"; // required DNS/IP identity for a client context
auto session = gamenet::rpc::RpcTlsSession::create(connection, tlsContext, {
    .ready = [] { /* authenticated: now call through session->channel() */ },
    .closed = [](gamenet::tls::TlsError reason) { /* terminal session event */ }
}, handler, options);
connection->setContext(session);
session->start();
```

Use `TlsContext::client("trusted-ca.pem")` on the client and a matching PEM
chain/private key through `TlsContext::server` on the server. All existing TLS
chain, SAN hostname/IP, expiry and minimum-version policies remain mandatory.
No verification bypass, automatic raw-TCP upgrade or mTLS is added.

Forward raw TCP Buffers to session `onMessage`, optional write notifications to
`onWritable`, and actual TCP-down to `onDisconnect` before clearing context.
Only session TLS-ready opens RPC admission. Before that, `channel()->call`
returns NotReady/id=0 without retaining a callback, pending reservation or timer.
TLS close seals admission immediately. All RPC wire goes through TlsStream;
the adapter has no raw-TCP send fallback.

The complete client/server composition and an independent OpenSSL client are
in `tests/integration/rpc_tls/test_rpc_tls_tcp.cpp`. The latter batches many RPC
responses in one TLS record then closes while the receiver's RPC frame budget
is one; every received response must settle before session closed.

## Transport and backpressure

`RpcChannel::createWithTransport` takes a focused `RpcTransport`: immutable owner
loop identity, owner-only NotReady/Ready/Closed observation, copied-byte send
admission and abort. The channel owns the transport; bundled TCP/TLS transports
weakly observe their connection/stream. State observation invokes no callbacks.
Send may report synchronous disconnect but must never deliver RPC input inline.
Transport exceptions reject send or are counted during abort rather than escape
the reactor. This seam does not activate the old ConnectionTransport proposal.

RPC pending count/wire bytes and parser budgets compose with TLS pending
plaintext count/bytes, fixed BIO capacities and TCP multi-level cipher output
budgets. A full TLS queue returns RPC SendRejected/id=0, refunds provisional
pending/timer state and invokes no completion. RPC adds no retry queue. Already
accepted TLS writes use its existing bounded cipher-drain retry. As before,
Accepted means local admission, not peer execution or acknowledgement.

TLS sending drives output without delivering application plaintext inside the
send/reply call stack. Buffered input resumes through the existing bounded TLS
retry or normal input/writable path. This prevents nested RPC input while a
budgeted RPC handler is replying. Terminal callbacks may still re-enter send.

Request timeout, local cancellation, pending limits and late-response handling
are the existing RPC contracts. Cancellation does not undo remote side effects
or retract TLS bytes. There is no automatic retry or exactly-once remote execution.

## Lifetime and shutdown

One owner loop performs all session/channel/TLS mutation, callbacks and
destruction. Foreign mutations return WrongThread before a temporary strong
owner-object lease; use `ownerExecutor()` to marshal owned values and check
bounded admission. No coroutine/thread/scheduler is introduced.

The session owns TLS. TLS callbacks own an acyclic bridge that owns the channel;
the RPC transport only weakly observes TLS. Timers remain weak. Accepted TLS EOF
leases and one bounded bridge settlement post keep already received input alive
after raw TCP-down clears the context. Session closed, rather than raw TCP-down,
ends application callback lifetime: callbacks retain their session/application
state and lease it across reentrant calls.

On TLS end, new calls are closed immediately, but received RPC frames may still
have an accepted budget continuation. The bridge waits for that dispatch/work
to retire, then settles remaining pending calls as Disconnected, clears inbound
reservations/timers and emits one closed callback with the TLS diagnostic. A
sealed transport rejects new incoming handlers while already received responses
can still complete their pending calls. The snapshot preserves both RPC results
and TLS/session terminal reasons. Settlement
admission rejection does not execute buffered handlers inline: it closes RPC and
reports SchedulingRejected. In that resource-failure path the terminal callback
may re-enter the active handler; the handler retains its own application state.

Explicit `shutdown` first seals RPC and settles outstanding calls as Shutdown,
then drains TLS close_notify. `abort` also settles RPC first, then forces TLS
teardown. Dropping an active session settles Shutdown/Aborted using TLS's
non-reentrant abandonment policy. Repeated down/close/abort is idempotent.
Close or abort sessions before quitting the owner loop. RPC callbacks and closed
exceptions are contained; ready exceptions become TLS CallbackException.

## Validation

Ordinary TCP/RPC CI remains 133 tests. TLS-enabled Release runs 138 tests; the
optional workflow selects exactly five real-OpenSSL TLS/RPC-TLS tests under
Linux ASan/UBSan, Linux TSan and Windows ASan. Installed consumers separately
verify TLS, RPC/TLS, Core-only and RPC-only discovery, plus OFF rejection.
RPC-C3 additionally executes the three ordinary RPC tests under each sanitizer;
the full Release inventory already includes them. RPC/TLS integration includes
eight fresh authenticated connection generations with send saturation, held-input
recovery, healthy probes, cancellation/late responses and pending disconnect.
Every generation settles once with zero RPC/TLS obligations and expires its weak
lifetimes before replacement. Prior-generation reply handles remain Closed.
No skipped/fallback test counts as evidence. Functional development validation
does not establish production capacity or satisfy HP0 promotion gates.
