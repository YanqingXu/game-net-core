# M9 TLS / WebSocket / DNS Promotion Audit

Date: 2026-08-24

Core baseline: `fff41622ffc1d2e0d047d3938529d9eb7919e5af`

## Decision

- shared transport/TLS/WebSocket/DNS promotion: `NO-PROMOTION`;
- `connection_transport`, `tls`, `websocket`, `dns_resolver`, and the HTTP
  upgrade dependency remain `deferred`;
- the active installed `TransportEndpoint` stays unchanged: it is an
  upper-layer endpoint adapter, not the proposed per-connection
  `ConnectionTransport` I/O seam;
- no Core TLS, WebSocket, DNS, HTTP-upgrade header, target, package component,
  compatibility promise, tag, or empty v0.8 release is created;
- M9 is closed and the unique governance front advances to M10 UDP/KCP
  experimental-capability evidence review.

The audit found one strong but consumer-private TLS implementation, not two
consumers of one substitutable owner, handshake, I/O, shutdown, credential,
reload, package, WebSocket, or DNS contract.

## Audited Inputs

### Current GameNet Core

Core keeps TLS explicitly disabled: `GAMENET_ENABLE_TLS=ON` fails configuration
with a deferred diagnostic. There is no installed TLS, WebSocket, DNS, or HTTP
upgrade surface.

The existing active `TransportEndpoint` contract is deliberately higher-level:

- `TcpTransportEndpoint` weakly observes one `TcpConnection` and exposes
  identity, owner executor, owner-thread send/close, typed cross-thread terminal
  close, and an open-state snapshot;
- it does not perform socket reads, handshakes, encryption, framing, or
  certificate management and does not own connection-private transport state;
- its owner/lifetime/dispatch contracts passed 3/3:
  `test_tcp_transport_endpoint_contract`,
  `test_tcp_transport_endpoint_lifetime`, and
  `test_tcp_transport_endpoint_dispatch`.

Those tests protect the current plain-TCP upper-layer boundary. They do not
prove the deferred `ConnectionTransport` seam or TLS behavior.

### `gamenet-game-gateway`

The exact gateway closure is
`588acd079be93de3e230ba4f07dd111f7bec6a3c`. Its tracked runtime has no TLS,
OpenSSL, WebSocket, DNS resolver, `getaddrinfo`, or `ConnectionTransport`
implementation. Its governed GameNet path remains official-package plain TCP
plus the external packet/RPC adapters, and its M7 scope explicitly excluded
TLS and WebSocket.

Gateway therefore supplies no first implementation of the proposed M9
contracts and cannot form one half of a two-consumer promotion pair.

### `YanGameServer`

The independent source checkpoint is
`b5254165389d762c3f3c63568c24ffab448fc501`. It was inspected and built from
the existing read-only archive without switching or modifying its dirty live
checkout.

YanGame has a substantial TLS subsystem, but its frozen contract is specific
to internal RPC:

- one dedicated endpoint/server worker exclusively owns a native TCP socket,
  `TlsContextManager`, OpenSSL provider, sessions, and a count-one rotation
  queue;
- TLS 1.3 mutual authentication and exact peer DNS verification complete
  before its node handshake and wire-v2 RPC frames;
- credential, context generation, session, ciphertext/plaintext, handshake,
  deadline, rotation, and shutdown work have YanGame-specific bounds and
  `Result<T>` errors;
- existing sessions retain an immutable credential generation while new
  sessions observe an activated rotation; failure preserves the last good
  generation and withdraws process readiness;
- its frozen intent explicitly excludes GameNet EventLoop TLS, external
  GameNet client-listener TLS, callback-driven OpenSSL, and a generic
  `TcpConnection` transport seam;
- no runtime WebSocket, DNS resolver, `getaddrinfo`, or
  `ConnectionTransport` implementation exists at the checkpoint.

The archived Windows Release contract-fallback configuration registered six
focused TLS tests. Credential bundle, context rotation, file source, and
disabled OpenSSL-adapter contracts passed 4/4; the real OpenSSL memory
handshake and rotation/session race tests were explicitly skipped by that
fallback configuration. YanGame's frozen stage document separately records
its historical external-OpenSSL 94/94 matrix, but this audit does not restate
that historical run as a current execution.

## Contract Comparison

| Boundary | Core / gateway | YanGameServer | M9 conclusion |
| --- | --- | --- | --- |
| Transport seam | Active upper-layer `TransportEndpoint`; no connection-private read/write seam | Native RPC endpoint/server workers directly drive sockets and TLS manager | Different layer and owner; no `ConnectionTransport` pair |
| TLS owner | No implementation; proposed owner is each TcpConnection EventLoop | Dedicated RPC transport worker owns manager/session/socket | Not substitutable |
| Handshake/protocol | Plain GameNet TCP and external adapters | mTLS 1.3, peer DNS, then node handshake and wire-v2 RPC | Consumer-private policy and ordering |
| I/O state | Production epoll/IOCP TcpConnection readiness/completion | Provider session ingests/drains ciphertext through worker polling | No shared WANT_READ/WANT_WRITE/partial-I/O contract |
| Credentials/reload | No context or reload contract | Bounded file bundle, current/candidate/draining generations, all-or-stop readiness | Only one consumer has evidence |
| Shutdown/re-entry | TcpConnection callback lifecycle on EventLoop owner | Worker-owned session close; provider adapter invokes no user callback | Different close and callback boundaries |
| Package/platform | TLS is OFF-only; no package component | Project-private optional OpenSSL/fallback targets | No dual-platform GameNet package consumer |
| WebSocket | No implementation | No implementation | Zero consumer pair |
| DNS | No implementation | No implementation | Zero consumer pair |

## Deferred Intent Review

The preserved M9 intents are design assets, not implementation authorization:

1. `connection_transport` describes the desired per-TcpConnection seam, but
   neither audited consumer implements that exact layer. It must not be
   confused with active `TransportEndpoint`.
2. `tls` still uses old `MINI_ENABLE_TLS`, exception, coroutine-transparency,
   and direct TcpConnection state assumptions. It does not yet close the M9
   gates for handshake timeout, renegotiation prohibition, callback exception,
   credential reload, or dual-platform installed-package consumption.
3. `websocket` says fragmentation is unsupported while M9 requires bounded
   fragmentation behavior, and it assumes a full `HttpServer` dependency
   instead of the required minimal external upgrade adapter. It also lacks a
   complete slow-client/output-backpressure contract.
4. `dns_resolver` describes a global shared instance and permits queued work to
   lose callbacks on resolver destruction and undefined delivery after loop
   destruction. It has no bounded admission, cancellation, or exact terminal
   settlement contract and depends on the unpromoted coroutine layer.
5. `http` remains a full-server design outside the v1 scope. It is not promoted
   merely to make a WebSocket upgrade possible.

Any future resume must first rewrite the affected intent against current Core,
name concrete typed admission/cancel/shutdown and owner-retirement contracts,
and provide two real consumers plus direct contract and installed-package
tests.

## Ownership, Re-entry, and Cross-Thread Decision

This audit creates no new owner or callback path. Core EventLoop/TcpConnection,
gateway network/logic-cell, and YanGame native RPC worker owners remain
separate. No OpenSSL handle, DNS request, WebSocket state, or connection-private
transport object becomes a cross-thread capability. Existing Core work
continues through the current owner-thread and typed bounded admission rules.

## Repository Verification

`tests/cmake/test_migration_status_contract.py` binds this audit, all five
deferred intent/catalog entries, exact external checkpoints, the 3/3 Core and
4/4-plus-2-skipped independent evidence, M9 `NO-PROMOTION`, M10 as the next
governance front, the distinction between `TransportEndpoint` and
`ConnectionTransport`, and the absence of installed Core TLS/WebSocket/DNS/HTTP
headers or CMake targets.
