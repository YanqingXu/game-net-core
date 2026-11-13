# Optional bounded standard KCP (KCP-C1)

`GameNet::kcp` is a provisional message transport over explicit UDP peers. Build
with both `-DGAMENET_ENABLE_KCP=ON -DGAMENET_ENABLE_UDP=ON`; both default OFF.
There is no OpenSSL, custom wire header or TransportEndpoint dependency.

```cmake
find_package(GameNetCore CONFIG REQUIRED COMPONENTS kcp)
target_link_libraries(your_server PRIVATE GameNet::kcp)
```

Create/start on an owner EventLoop, exchange the known remote UDP address and
nonzero conv out of band, then call `openSession(address, conv)` at both ends.
The address/conv is not authenticated. Select a fresh conv for each replacement,
including across process restarts. Each transport refuses conv reuse and caps
its history; recreate it with a fresh application-managed conv namespace after
history exhaustion. There is no peer discovery or KCP close handshake.

`session->send(payload)` accepts complete copied messages, including empty ones,
and returns an id. Rejection returns id zero with no reservation or callback.
Accepted messages terminate exactly once locally: Acknowledged means cumulative
KCP ACK passed all fragments, Cancelled means local close/stop, Failed means a
deadline, resource, scheduling or transport failure. ACK does not promise remote
application processing. Close preserves already committed ACK results.

Creation/start/open/send/close and snapshots are owner-only. `requestClose`,
transport stop and wrapper destruction may run cross-thread and seal admission
before returning. Callbacks run on owner outside locks and native KCP; they may
reenter/throw/release. Destroying a running wrapper requests stop. Keep the owner
alive and draining until stopped, which includes UDP/IOCP terminal retirement.
Old handles never acquire a replacement generation.

Default MTU 1200, send window 32, receive window 128, send-segment cap 64 and ACK
cap 256 are validated. Message size defaults 16 KiB, per-session pending count
32/bytes 64 KiB, aggregate count 256/bytes 1 MiB, sessions 32, conv history 4096,
protocol arena aggregate 16 MiB. Each engine reserves its entire fixed arena and
scratch up front (about 440 KiB at defaults; exact size depends on ABI). Pending
metadata and UDP copied/in-flight output have independent count/byte limits.
Empty messages consume count and segment capacity. Configured bounds include
closing sessions until their original native obligations retire.

Input validates the whole standard datagram before mutation. ACK pressure drops
it for retransmission. ACK timestamps older than the maximum upstream RTO of
60 seconds are rejected before the signed RTT filter can overflow. UDP output
refusal is wire loss with no secondary queue.
The standard congestion controller remains enabled. Default update interval is
20 ms, send lifetime 10 seconds, idle timeout 30 seconds, and retransmission limit
10. An unreachable peer eventually fails; the result cannot disambiguate remote
delivery followed by lost ACK from no delivery. Reconnect/retry policy is the
application's responsibility.

The unchanged MIT upstream is fixed at
[`b1a7a2101dcbb96017681a500d6b82bbe5a88766`](https://github.com/skywind3000/kcp/commit/b1a7a2101dcbb96017681a500d6b82bbe5a88766).
`third_party/kcp/upstream.json` records exact raw-byte hashes, including original
line endings. Private symbol prefixes isolate allocator hooks from another
upstream copy. The unchanged source compiles as C++ so fixed-arena exhaustion
unwinds to the adapter. Its C null-base container-offset macro is overridden
with standard `offsetof` in compilation wrappers (including the independent
reference); no algorithm or wire format changes and no sanitizer suppression.
Interrupted native mutation
discards the entire arena without following partial lists. MSVC disables `/EHc`
for this target so allocator exceptions cross the private C-linkage boundary.

Verification uses three CTest tests (141 with UDP, versus default 133), real
IPv4/IPv6 pressure/reconnect, a separate unchanged-upstream/native-socket peer
through bounded bidirectional data/ACK loss, duplication, reordering and delay,
and protocol/session fuzz at 20k each. The dedicated workflow records Windows/
Linux Release, Windows ASan, Linux ASan/UBSan and TSan evidence, package consumers,
clean source identities and all raw hashes. Existing optional protocol checks
remain separate. These contracts are not capacity or production validation.
