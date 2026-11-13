# WSS close interoperability hardening

The follow-up authorized on 2026-11-13 retains the installed APIs, default-OFF
options, ownership rules and Core / WS / OpenSSL dependency boundaries. The
change is in independent validation and its non-installed server diagnostics.

## Observed failure and cause

PR #28 Windows ASan and main Windows Release intermittently failed the pinned
synchronous client's Close(1000) assertion. Native tests passed; rerunning did
not explain the failure. The diagnostic branch at `00f8cee` captured the same
failure on [Windows ASan](https://github.com/YanqingXu/game-net-core/actions/runs/43346585090/job/131627304901),
after 99 successful generations. The next client sent Close(1000, done), received
no Close, reported 1006 and retained `TimeoutError('The read operation timed out')`.
Its correlated server trace had received/admitted/encrypted the Close reply;
the client error occurred within the same clock tick as close initiation.
The companion Release diagnostic completed 256 generations. Both artifacts and
their ZIP/file hashes are retained as original evidence.

The synchronous reader begins an SSLSocket read with an infinite blocking
timeout. Another thread starts close and changes the socket timeout to a
positive value. In CPython's read retry, the cached negative timeout reaches
PySSL_select while the live socket timeout is positive; it is interpreted as
expired. The pinned synchronous WebSocket receive loop converts this false
read timeout into EOF and 1006. See the exact
[CPython 3.12.10 implementation](https://github.com/python/cpython/blob/v3.12.10/Modules/_ssl.c#L2128)
and [pinned client close path](https://github.com/python-websockets/websockets/blob/15.0.1/src/websockets/sync/connection.py).

`tests/ci/wss_timeout_repro.py` proves this with a real authenticated TLS peer.
The test-only SSL message callback holds the reading thread at a TLS record
header, after the blocking timeout is captured. The controller changes the
timeout to three seconds, then releases that barrier while withholding the
record body. The read reports TimeoutError before body release. Windows
CPython 3.12/3.14 and Linux CPython 3.12 reproduce the same sequence, with no
remaining worker. Timings in this proof diagnose ordering, not performance.

## Fix and regression contract

The pinned websockets 15.0.1 qualification peer now performs TLS on one asyncio
loop. The three-second bounds are retained. `wss_async_peer.exchange` strictly
checks text, binary, Pong, Close(1000, done), both Close frame directions and
absence of SSL/parser errors. The library production implementation is unchanged.

Each of three independent rounds requires:

- Eight healthy generations: 16 fragmented text/binary exchanges, eight Pongs
  and eight fully graceful WS/TLS terminals.
- One independent stdlib masked Close plus unwrap: one more graceful terminal.
- One explicit masked Close followed by raw FIN without close_notify: a
  truncated terminal. Certificate failures are counted separately.
- Unknown CA, wrong hostname, expired certificate and HTTP policy rejection.

The existing WSS native contract additionally controls separate TLS records,
simultaneous Close, immediate or deferred close_notify, notify without Close,
an incomplete Close frame and raw FIN without a TLS alert. Clean sequences
require the Close echo and both alerts; incomplete sequences remain failures.
Every case has one terminal and zero pending WS/TLS ledgers.

Client diagnostics bound retained frames and include Close code/reason,
sent/received frames, runtime, TLS identity and receive/SSL exceptions. Server
diagnostics use weak observations and dispatch leases, correlate by peer port,
and record WS/TLS/TCP state plus accepted/encrypted/discarded/pending bytes.
Negative-evidence guards reject missing frames, wrong codes, duplicate terminals,
missing alerts, retained bytes, broken ledgers and incomplete round/proof data.

The `gamenet.websocket_tls_contracts.v2` manifest binds exact source/run/attempt,
inventories, JUnit, all three peer rounds, the separately labelled legacy proof
and file hashes. Qualification still requires Windows/Linux Release, Windows
ASan, Linux ASan/UBSan/TSan, installed ON/OFF consumers and existing fuzz gates.
The manual diagnostic probe is investigation evidence, not a substitute for
normal process shutdown or the complete qualification driver.
