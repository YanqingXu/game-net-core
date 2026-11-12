# Bounded WebSocket server (WS-C1)

## WS-C3 state and pressure validation

The existing session contract runs explicit transition seeds and 32 fixed
512-byte sequences through a shared non-installed runner. Each input uses fresh
session generations on a real EventLoop, at most 128 actions, one/four-frame
parser budgets, finite transport/control/input limits and hour-long deadlines.
The driver explicitly wakes its initial owner post before entering the loop;
subsequent actions and parser continuations interleave through bounded admission.
No sleep or short wall-clock timeout selects fuzz outcomes. Existing deadline
contracts continue to verify real timeout behavior.

Actions exercise partial Upgrade, fragmented/control/malformed input, readiness,
application refusal/recovery, close/down/abort/abandon, queue saturation,
synchronous terminal send reentry, nested input, callback exceptions/release and
generation replacement. Independent receipts validate canonical unmasked server
frames and accepted = drained + discarded + pending transport bytes. Every
action checks callback identity, no message after terminal, control conservation,
caps and zero terminal input/fragment/control. Final owner drain makes accepted
obsolete work harmless and all retired weak sessions expire.

The real TCP integration additionally executes eight pressure generations with
a masked independent wire peer. Every generation must actually reject a burst
under finite TCP output admission, resume bounded test-held input, recover every
accepted payload and complete a fresh probe. Clean WS Close and pending-output
disconnect alternate. A clean TCP EOF without the WS Close is truncated.
Closed old session handles/feed/down/writable callbacks are replayed while the
replacement is live; no new-generation counter or callback may change. Deliberate
stale leases are then released and all old session/connection weak handles retire.
This finite correctness test does not assert production endurance or capacity.

The existing six-lane optional workflow now runs two instrumented fuzz targets:
protocol bytes and session actions, each exactly 20,000 units at seed 424242.
State evidence retains 40 original transition/fixed seeds, evolved corpus,
command, binary hash, empty crash evidence, source/run identity and file hashes.
A separate readback step rejects missing/changed inputs, command/count/seed drift,
dirty hosted checkout and sanitizer diagnostics. Nested state manifests are also
included in the parent artifact hash list. Default 133, WS-only 136, TLS-only
138 and combined 143 registered test counts remain unchanged.

`GameNet::websocket` is a provisional, default-off component. Enable
`GAMENET_ENABLE_WEBSOCKET=ON`, install, then request it explicitly:

```cmake
find_package(GameNetCore REQUIRED COMPONENTS websocket)
target_link_libraries(app PRIVATE GameNet::websocket)
```

The separate export has only a Core dependency. Core-only discovery never loads
WebSocket, and WebSocket requires neither HTTP nor OpenSSL. An OFF installation
contains no WebSocket headers/export and rejects a required component request.
Published v0.3.1 assets predate WS-C1; use the development revision.

## Minimal server

```sh
cmake -S . -B build-ws -DGAMENET_ENABLE_WEBSOCKET=ON
cmake --build build-ws --config Release --target gamenet_websocket_echo_server
# Single-config Linux:
./build-ws/examples/gamenet_websocket_echo_server 7001
# Windows multi-config:
./build-ws/examples/Release/gamenet_websocket_echo_server.exe 7001
```

Connect to `ws://127.0.0.1:7001/echo`. The example binds loopback, echoes text
and binary messages, and uses a finite TCP output budget. Port 0 chooses an
ephemeral port and prints `PORT <n>`; an optional second argument stops the
server after that many seconds. The echo example uses a 256-KiB frame/message
limit. Application send refusal closes with 1013 rather than retaining a queue.

## Session composition

Configure TcpServer callbacks before start. On TCP-up, create a
`WebSocketSession`, store its shared pointer in the connection context and call
`start()`. Forward TCP message Buffers to `onMessage`, optional write-complete
notifications to `onWritable`, and TCP-down to `onDisconnect` before clearing
context. `examples/websocket_echo/main.cpp` contains the complete composition.
No Core callback slot is replaced by the session itself.

TCP-up starts the bounded HTTP handshake. Application admission opens only after
the complete valid Upgrade and successful TCP admission of the 101 response.
`handshake(request)` can check target, Host and Origin and return false for 403.
The caller supplies its application Origin/authentication policy. Extensions and
subprotocols are not selected. The implementation is a strict Upgrade parser;
general HTTP routing, request bodies, HTTP clients and WSS are separate features.

HTTP parsing rejects ambiguous length/transfer headers, folded fields, controls,
duplicate singleton fields, malformed keys and invalid token lists. It accepts
only origin-form GET HTTP/1.1, canonical 16-byte base64 keys and version 13.
Invalid requests receive bounded 400/403/426/431 responses. Header size/field
count and handshake duration are limited. Successful Upgrade preserves trailing
WebSocket frame bytes from the same TCP input.

## Frames and limits

The codec receives masked client frames and emits unmasked server frames. It
enforces canonical lengths, RSV/opcode/control rules, fragmented message order,
message/frame/input caps, UTF-8 text and close reasons, and allowed close codes.
Ping/Pong/Close can occur between data fragments. Control payloads are at most
125 bytes. Text/binary callbacks receive one reassembled message, borrowed only
until that callback returns. Copy data before retaining it or crossing threads.

`send(Text|Binary|Ping, bytes)` returns a typed result. Accepted hands the encoded
frame to the existing TCP output budgets; it does not acknowledge peer delivery.
NotReady/Closed/Overloaded/InvalidArgument retain no application output. Server
application frames are emitted unfragmented and must fit `maxFrameBytes`.
There is no application retry queue. Upgrade, automatic Pong and Close use a
separately bounded control queue with one weak retry timer. Retry also works
without optional write-complete notifications. Saturation terminates explicitly.
Starting Close discards/account pending Pongs and reserves the bounded Close.

The snapshot reconciles enqueued control bytes to admitted + discarded + pending.
Input bytes, fragment bytes, control bytes/count, retries and send/callback
counters remain observable. Terminal cleanup returns logical input/fragment and
control counts to zero. This is bounded correctness, not a fixed-lab performance
or production capacity claim.

## Ownership and terminal behavior

All session mutations, snapshots, callbacks and destruction belong to the TCP
owner EventLoop, which must outlive the session and accepted work. Foreign
operations reject WrongThread before a self lease; marshal owned values through
`ownerExecutor()` and check bounded admission. The context/caller owns the session;
it weakly observes TCP. Timers are weak; dispatch and at most one continuation
lease owned state. Recursive input leaves the nested Buffer untouched. Ready,
message, pong, handshake policy and closed callbacks may re-enter owner operations
and release caller references; exceptions are contained.

Close seals new sends, drains a valid Close, receives peer Close and then submits
TCP shutdown. Handshake/close have deadlines. Graceful requires both Close frames
and actual TCP-down with no pending control output. EOF without a completed close
is truncated; protocol faults preserve 1002/1007/1009 diagnostics. Already received
complete events retire through the normal budget at EOF even after context
release. Rejected continuation is terminal SchedulingRejected. Explicit abort
forces TCP teardown; dropping an active session settles Aborted using non-reentrant
TCP shutdown. Close/abort before stopping the owner. Closed state, buffer cleanup
and timer cancellation precede one terminal callback; repeated down/close is safe.

## Validation

Default Core CI remains 133 tests and TLS-only builds remain 138. A standalone
WebSocket-enabled build has 136 tests. The optional workflow executes full
Windows/Linux Release, three selected tests under Linux ASan/UBSan, Linux TSan
and Windows ASan, and a dedicated 20,000-unit Clang ASan/UBSan protocol fuzz lane.
All five non-fuzz lanes run an independent pinned `websockets==15.0.1` client
through eight reconnects, fragmented Unicode/binary messages, Ping/Pong, Close
and policy rejection. Exact inventories/JUnit, interoperability, installed ON/OFF
isolation, source/run identity and evidence-file hashes are retained. No skipped
or fallback result counts as a pass. The server is not a general HTTP framework.
