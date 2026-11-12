# Optional bounded UDP — UDP-C1

UDP-C1 adds provisional `GameNet::udp`. Enable `GAMENET_ENABLE_UDP=ON`; the
default build, installed Core discovery and existing TCP/TLS/WS use stay
unchanged. UDP requires no OpenSSL or reliability library. The published v0.3.1
packages predate this component. This is native development functionality with
correctness evidence, not a capacity or production-adoption claim.

```sh
cmake -S . -B build-udp -DGAMENET_ENABLE_UDP=ON
cmake --build build-udp --config Release
ctest --test-dir build-udp -C Release -L '^udp$' --output-on-failure
```

Installed consumers request `find_package(GameNetCore CONFIG REQUIRED COMPONENTS
udp)` and link `GameNet::udp`. Core-only discovery does not load UDP, even from
an ON package. OFF packages contain no UDP headers/export and reject a required
UDP component for the explicit missing-component reason.

## Ownership and callbacks

Create/bind, `start`, `localAddress` and `snapshot` run on one outliving owner
EventLoop. Each object starts once. `sendTo`, `stop` and wrapper destruction are
thread-safe; destruction requests stop. Keep the owner running until stopped
and native completions retire, then release the object before destroying the
loop. The loop's quit participant also closes admission and cancels operations.
Stop before start releases the unregistered socket without user callbacks.

```cpp
gamenet::net::EventLoop loop;
std::shared_ptr<gamenet::udp::UdpSocket> socket;
socket = gamenet::udp::UdpSocket::create(
    &loop, gamenet::net::InetAddress(9000, true), {
        .datagram = [&](const auto& peer, auto bytes, bool truncated) {
            if (!truncated) {
                const auto result = socket->sendTo(peer, bytes);
                // Inspect result.result; rejected sends have id == 0.
                (void)result;
            }
        },
        .stopped = [&](auto) { loop.quit(); }
    });
if (socket->start() == gamenet::udp::UdpResult::Accepted) {
    // Application control calls socket->stop() when service should end.
    loop.loop();
}
```

All callbacks run on the owner and may send, stop, throw or release the last
caller reference. Datagram callback commitment shares the stop admission lock:
stop prevents new callback commitment while an already committed owner frame
may finish. Datagram peer/payload views expire at callback return. Copy
data before retaining or handing it elsewhere. Exceptions close admission while
remaining accepted sends still settle. Stopped is emitted once after queue,
native operations and reservations are zero. The callback set is then retired;
application captures should avoid creating their own strong ownership cycles.

## Datagram and budget contract

IPv4 and IPv6 are separate sockets; IPv6 explicitly enables IPV6_V6ONLY. Empty
datagrams are real data. Receive size is bounded by `maxDatagramBytes` (default
1200, maximum 65507). Truncation reports a bounded prefix and `truncated=true`;
the unknown original length is not reported. Following packets remain usable.

Every admitted send owns a copy and nonzero id. Both packet and byte limits
include in-flight output; an empty packet still consumes one packet slot.
Rejected sends retain no data and have no completion. Accepted sends receive
one `Sent`, `Cancelled` or `Error` result after releasing reservations. `Sent`
means local OS completion; UDP does not promise peer delivery/order/uniqueness.
Receive connection-refused/reset indications are counted as per-datagram diagnostics and allow subsequent valid packets to recover. Other permanent receive errors stop the socket explicitly.
Cancellation does not prove that no datagram reached the network before stop.
Callers decide how to react to `Overloaded`; no fallback queue is created.

Read and send batches each have finite packet and byte caps. Byte caps must
fit one configured datagram. Linux returns to epoll at would-block or budget
exhaustion. Windows has one receive and one send in flight and consumes native
completion identity under the existing bounded owner phases. Storage is leased
before the native call; synchronous rejection releases that lease without a
phantom completion. Stop removes observation and closes the socket, while
OVERLAPPED/address/buffer/payload storage survives until terminal dequeue.

Snapshots independently reconcile packet and byte totals:
`accepted = sent + cancelled + failed + pending`. They expose receive/truncation,
batch maxima, callback exceptions and native-error diagnostics. At Stopped,
pending packets/bytes, receive storage and native operations are zero.

## Minimal server and verification

`gamenet_udp_echo_server <4|6> <port> <message-count> <maximum-datagram-bytes>`
binds loopback and prints its actual port. A zero count runs until application
stop/process termination. A positive count stops after that many local send
completions. It echoes the bounded prefix of a truncated packet and reports
truncation in its final summary, making that behavior explicit for diagnostics.

```sh
python tests/ci/udp_interop.py --binary build-udp/examples/gamenet_udp_echo_server --output out/udp-peer
python tests/ci/run_udp_contracts.py --profile release --build-dir build-udp-validation --output out/udp-validation
```

The independent stdlib peer uses IPv4/IPv6, eight fresh server generations,
48 datagrams including eight empty and sixteen truncated inputs, binary/exact
payloads and recovery probes. It verifies payload boundaries, source identity,
server result and zero-cancellation summaries without GameNet parsing helpers.

Three registered tests cover options, native datagram/lifetime contracts and
16-generation real burst/fairness/restart pressure. They require actual queue
refusal followed by recovery, packet/byte caps, timer/second-socket service,
multiple producers, cross-thread stop and queue saturation, callback reentry/
exceptions/reference release, stop/start races and eventual weak-state expiry.
Windows additionally injects pre-submission allocation/native rejection and
stops while real read and write operations await terminal consumption.

The dedicated workflow runs Windows/Linux Release (136 tests each), Windows
ASan and Linux ASan/UBSan/TSan (three UDP tests each), independent peers and
installed consumers. Release lanes additionally build physical OFF packages.
Exact commit/run/attempt, commands, JUnit, logs, executable/file hashes and
interop counts are retained. Existing Core/TLS/WS/WSS/fuzz lanes remain separate
regression gates. These finite runs do not replace long-duration or HP0 gates.
