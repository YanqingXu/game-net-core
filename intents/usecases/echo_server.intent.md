---
status: active
target: gamenet_echo_server
migration_source: mini_trantor
promote_gate: none
artifact_kind: example
---

# Usecase Intent: Echo Server

## 1. Intent
Build a minimal echo server on top of `GameNet::core` to validate:
- accept path
- connection creation
- read callback
- write/send path
- connection teardown

## 2. Why It Exists
The echo server is the first full-path integration usecase.
It validates whether the reactor core can support a real connection lifecycle.

## 3. Scope
- one base EventLoop is sufficient for the example
- plain TCP only
- no protocol framing
- no coroutine or business protocol behavior
- command-line input is limited to an optional listen port

## 4. Expected Flow
1. accept new connection
2. create TcpConnection
3. register Channel
4. on readable, read bytes
5. send back same bytes
6. on peer close/error, teardown safely

## 5. Validation Goal
This usecase should validate:
- EventLoop dispatch works
- Poller registration works
- Channel callback path works
- connection teardown path is safe
- the received byte sequence is echoed unchanged

## 6. Threading and Ownership
- `main()` owns EventLoop and TcpServer and destroys both on the owner thread
- TcpServer owns accepted connection bookkeeping
- connection and message callbacks run on each TcpConnection owner loop
- the example performs no direct cross-thread mutation
- Buffer bytes are retrieved by the message callback and copied into `send()`

## 7. Contracts
- `examples/echo_server/main.cpp` is the minimal runnable server
- `tests/integration/tcp/test_tcp_server_client_echo.cpp` verifies the real
  TcpServer/TcpClient loopback connection and byte-for-byte echo path
- benchmark and protocol behavior belong to their own intents and must not
  accumulate in this example
