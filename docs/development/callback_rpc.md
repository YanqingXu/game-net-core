# Callback RPC (RPC-C1)

RPC-C2 adds the focused RpcTransport seam and optional separately installed
[RPC/TLS session](callback_rpc_tls.md). Existing TCP composition below is preserved.

`GameNet::rpc` is a provisional installed C++23 component. Link it with
`find_package(GameNetCore REQUIRED COMPONENTS rpc)` and
`target_link_libraries(app PRIVATE GameNet::rpc)`. The RPC component depends on
Core and PacketFramer; the stable Core does not depend on RPC. The published
v0.3.1 package predates this feature: build/install the PR revision to try it.

The component is a native minimal callback protocol, independent of historical
gateway/YanGame wire formats. It provides one channel per TCP connection, opaque
binary payloads and numeric method IDs. Applications own service dispatch.
The runnable end-to-end composition is `tests/integration/rpc/test_rpc_tcp.cpp`:

```sh
cmake -S . -B build-rpc
cmake --build build-rpc --config Release
ctest --test-dir build-rpc -C Release -R '\.rpc\.' --output-on-failure
```

## TCP composition

Configure TcpServer/TcpClient callbacks before connecting. On connection-up,
create a channel on that connection's owner; a server handler can be:

```cpp
auto channel = gamenet::rpc::RpcChannel::create(connection,
    [](const gamenet::rpc::RpcMessage& request, gamenet::rpc::RpcChannel::Reply reply) {
        if (request.method == 1)
            (void)reply({gamenet::rpc::RpcStatus::Ok, request.payload});
        else
            (void)reply({gamenet::rpc::RpcStatus::RemoteError, "unknown method"});
    });
connection->setContext(channel);
```

The enclosing server/client message callback takes a temporary copy of the
context's shared channel and calls `channel->onMessage(*buffer)`. Its down
callback calls `channel->onDisconnect()` before clearing context. These are
application callbacks; the RPC channel never changes TcpConnection callback slots.
A client on its owner calls:

```cpp
auto admitted = channel->call(1, "hello", std::chrono::seconds(2),
    [](gamenet::rpc::RpcResult result) {
        // Ok, RemoteError, Timeout, Cancelled, Disconnected, Shutdown,
        // ProtocolError or SchedulingRejected. Executed on the connection owner.
    });
if (admitted.status == gamenet::rpc::RpcOperation::Accepted)
    (void)channel->cancel(admitted.requestId); // optional, owner-only
```

Rejected admission has id=0 and invokes no completion. The caller checks the
typed reason (pending limit, transport rejection, invalid argument, unavailable
owner, closed channel or exhausted IDs). `call` can complete synchronously only
through subsequent explicit owner operations such as cancel; an admitted timer
or peer response runs later. Pending count and charged wire bytes return to the
budget before completion, allowing callback re-entry. Completion exceptions are
contained and counted so other requests still settle.

## Deferred handlers and limits

A handler may copy its weak Reply and return. Reply must be invoked on the
connection owner before the request deadline; use the existing bounded executor
to return owned results from another thread and check PostResult. The first
reply consumes the inbound slot. Repeated/expired replies return NotFound;
closed/destroyed channels return Closed. Handler exceptions produce a bounded
remote error. Input payload references are valid only during the handler:
copy/move application data before deferring it.

`RpcOptions` independently bounds outgoing count/charged bytes, inbound deferred
count/charged bytes, payload size, parser buffering, frames per dispatch and
maximum request duration. Charged bytes are outer prefix + RPC header + request
payload until settlement; callback/application allocations are caller-owned.
Transport bytes also remain subject to existing TCP multi-level output budgets.
A handler must be short/nonblocking; RPC does not create a worker or preempt it.
Deadline callbacks cannot run while the owner is blocked. Response/reply handling
still checks steady_clock to reject overdue work before timer delivery.

Cancellation is local: it releases the callback/slot and ignores later replies,
but does not send a remote cancellation or undo side effects. No retries occur.
Incoming requests use monotonic non-reused IDs; stale request IDs or malformed
frames are protocol errors. Unknown/duplicate responses are ignored. Exactly-once
completion does not establish exactly-once execution across reconnects.

## Shutdown and threading

All mutation and snapshots are owner-only; foreign call/cancel/reply/close return
WrongThread. Cross-thread callers post copied/moved values via ownerExecutor;
never capture borrowed spans. The caller/context owns the channel. Timers and
reply handles are weak, and queued continuations temporarily retain the channel.
The EventLoop must outlive it. Close all channels before quitting the loop; close
seals RPC admission and settles pending callbacks as Shutdown, while the caller
controls TCP drain/force-close. A down hook settles as Disconnected. Owner-thread
destruction is a final Shutdown settlement path, with exceptions contained.

```mermaid
stateDiagram-v2
    [*] --> Pending: accepted call
    Pending --> Terminal: response / remote error
    Pending --> Terminal: deadline / local cancel
    Pending --> Terminal: disconnect / close / protocol failure
    Terminal --> [*]: erase + release budget + one callback
```

Wire details and the five module gate answers are in the active RPC intent.
RPC-C1 evidence is functional development validation. External adoption,
fixed-lab performance and a future release remain separate work.
