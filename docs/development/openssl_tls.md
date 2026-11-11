# Optional OpenSSL TLS (TLS-C1)

Build with `-DGAMENET_ENABLE_TLS=ON` and OpenSSL 3 development files. On Windows,
set `OPENSSL_ROOT_DIR` and put that provider's DLL directory on PATH. The OFF
default never discovers OpenSSL and installs no TLS headers/library/export.
The provisional component has a separate export/dependency boundary:

```cmake
find_package(GameNetCore REQUIRED COMPONENTS tls)
target_link_libraries(app PRIVATE GameNet::tls)
```

Even a TLS-enabled installation can serve a Core-only consumer without finding
OpenSSL. The published v0.3.1 assets predate this feature; build/install this
development revision. External OpenSSL binaries are not bundled by this target.
Use a supported, security-updated provider/distribution for deployment.

## Compose with TCP

Configure the TcpServer/TcpClient message, connection and write-complete callbacks
before starting TCP. Create a TlsStream on TCP-up, put it in your connection
context, then call start(). Forward raw message Buffers to onMessage(), optional
write-complete notifications to onWritable(), and TCP-down to onDisconnect().
Never change Core callback slots after establishment. The integration test
contains a complete composition and an independent SSL_set_fd client:
`tests/integration/tls/test_tls_tcp.cpp`.

```cpp
auto context = gamenet::tls::TlsContext::client("trusted-ca.pem");
auto stream = gamenet::tls::TlsStream::create(connection, context, {
    .ready = [&] { /* now application TLS writes may be admitted */ },
    .data = [](std::string_view bytes) { /* copy before retaining/posting */ },
    .closed = [](gamenet::tls::TlsError error) { /* one terminal result */ }
}, "localhost");
connection->setContext(stream);
stream->start();
```

Contexts are immutable after construction. Client contexts always verify the
chain; each stream requires an expected DNS/IP identity. Certificates need a
matching SAN; DNS sends SNI and partial wildcards/CN fallback are disabled.
Explicit CA PEM gives portable trust on Windows. Empty client trust selects
OpenSSL's configured default paths, not an assurance of native Windows store
integration. Server contexts accept a PEM chain and matching unencrypted key;
encrypted keys are rejected without an interactive password prompt.
TLS 1.2 is the minimum, with compression/renegotiation/tickets disabled.

TCP-up does not mean TLS-ready. Send application bytes only through
TlsStream::trySend after ready; raw TcpConnection sends carry ciphertext here.
This component does not transparently reroute the existing callback RpcChannel,
add STARTTLS, or provide mTLS/credential reload. RPC-over-TLS needs a separately
governed protocol/transport adapter.

## Bounds, owner and callbacks

One connection EventLoop owns all SSL/BIO operations and callbacks. No socket
is handed to SSL: a fixed-capacity BIO pair exchanges ciphertext with Core's
existing epoll/IOCP transport. Contexts may be prepared on another thread before
publication; stream mutations return WrongThread off-owner. Marshal owned values
through the existing bounded executor and check admission.

Defaults bound each BIO direction to 32 KiB, queued cipher input (including the
read BIO) to 256 KiB, and queued plaintext to 1 MiB / 256 writes. TCP admission
adds existing connection/loop/server/global output limits. OpenSSL's record and
certificate-processing buffers are additional library storage; certificate lists
are capped at 64 KiB. maxStepsPerDrive is 64. Configure maxCipherSendChunkBytes
(default 4096) to fit the underlying TCP output limits. One weak timer retries
blocked work (default 5 ms), so dropped optional TCP notifications cannot strand
accepted TLS writes. There is no unbounded per-record post or busy spin.

trySend copies before Accepted. Overloaded/NotReady/Closed rejects own nothing.
Accepted queued bytes encrypt before local close_notify, or contribute to
discardedPlaintextBytes on failure. encryptedPlaintextBytes means handed to TLS,
not application-level acknowledgement by the peer. WANT writes retain their
same buffer and length until the operation completes.

pauseRead stops application plaintext delivery. It does not add a Core socket
read switch: ciphertext still has a finite aggregate queue/BIO admission cap.
resumeRead processes buffered records. A sustained paused producer exceeding
that cap closes with InputLimit; raising the cap cannot create unbounded storage.
Handshake work is independent of application pause. A paused closing reader must
resume to process peer records before the shutdown deadline.

Ready/data/terminal callbacks are owner-only and may send, pause/resume, close,
abort and release caller references. SSL dispatch never recurses. Callback data
is borrowed only during the callback. Exceptions are contained and terminal
cleanup precedes the single closed callback.

## Shutdown and EOF

shutdown seals application send admission, drains accepted plaintext, sends
close_notify, reads remaining peer application records and receives peer
close_notify before normal TCP shutdown. Default handshake and shutdown deadlines
are 10 and 5 seconds. Abort, protocol/verification/input failure, timer rejection
or timeout use the normal TCP forced-close path. EOF without a complete TLS
closure is Truncated; it cannot silently become a successful close.

TCP EOF can arrive while previously received cipher records await their TLS
work budget. One accepted bounded owner continuation temporarily retains the
stream while those records settle, even if the TCP context is cleared. TLS
closed, rather than raw TCP-down, is the terminal application-lifetime event.
Callback closures must retain their application/session state until then.
If continuation admission fails, SchedulingRejected settles the stream. Owner
shutdown must explicitly close/abort streams before quitting the EventLoop.

Destroying an unfinished stream reports Aborted and seals TCP output with
shutdown, avoiding synchronous TCP-down re-entry through a context deleter.
Call abort before dropping the final reference for immediate forced teardown.
No callback may dereference an object already being destroyed.

The protocol handling follows OpenSSL's [bounded BIO pair](https://docs.openssl.org/3.0/man3/BIO_s_bio/),
[write retry](https://docs.openssl.org/3.0/man3/SSL_write/),
[shutdown](https://docs.openssl.org/3.0/man3/SSL_shutdown/) and
[peer identity](https://docs.openssl.org/3.0/man3/SSL_set1_host/) contracts.

## Verification

Default CI remains 133 tests. TLS-enabled Release has 136; the optional workflow
also executes exactly three real-OpenSSL tests under Linux ASan/UBSan, Linux
TSan and Windows ASan. No fallback or skipped TLS test counts as evidence.
Fixtures generate an ephemeral CA and SAN certificates, including expired and
untrusted cases. Tests cover binary echo, fragmented handshakes/WANT retries,
count/byte saturation, dropped notifications, read pause/resume, cipher overflow,
callbacks, deadlines, close_notify, buffered EOF drain and truncation. An
independent OpenSSL socket client exercises TLS 1.2 interoperability.
Installed consumers prove ON TLS linkage and Core-only isolation; Release lanes
also build/install OFF and reject an explicit TLS component request.
