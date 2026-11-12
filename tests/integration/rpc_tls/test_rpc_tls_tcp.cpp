// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/rpc_tls/RpcTlsSession.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/core/net/SocketsOps.h"
#include "gamenet/protocol/PacketFramer.h"
#include "support/TlsCertificates.h"
#include "support/TestAssert.h"
#include "support/RpcReconnectStress.h"
#include <openssl/ssl.h>
#include <any>
#include <atomic>
#include <thread>
#include <cstdio>

using namespace gamenet::rpc;
using gamenet::tls::TlsContext;
using gamenet::tls::TlsError;
using namespace std::chrono_literals;
namespace {
std::shared_ptr<RpcTlsSession> session(const gamenet::net::TcpConnectionPtr& connection) {
    const auto* value = std::any_cast<std::shared_ptr<RpcTlsSession>>(&connection->getContext());
    return value ? *value : nullptr;
}
template<class Tcp> void wire(Tcp& endpoint) {
    endpoint.setMessageCallback([](const auto& connection, auto* input) {
        if (auto current = session(connection)) (void)current->onMessage(*input); else input->retrieveAll();
    });
    endpoint.setWriteCompleteCallback([](const auto& connection) {
        if (auto current = session(connection)) current->onWritable();
    });
}
void down(const gamenet::net::TcpConnectionPtr& connection) {
    if (auto current = session(connection)) current->onDisconnect();
    connection->setContext(std::any{}); // TLS/RPC bridges must outlive raw TCP-down if needed
}
void sessionEcho(gamenet::test::TlsCertificates& cert) {
    gamenet::net::EventLoop loop;
    unsigned completed = 0, clientClosed = 0, serverClosed = 0, ready = 0;
    const std::string payload = std::string("encrypted\0", 10) + std::string(65536, 'r');
    auto serverContext = TlsContext::server(cert.certificate.string(), cert.key.string());
    auto clientContext = TlsContext::client(cert.ca.string());
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "rpc-tls-echo");
    wire(server);
    RpcTlsOptions options; options.rpc.maxPayloadBytes = 128 * 1024;
    server.setConnectionCallback([&](const auto& connection) {
        if (!connection->connected()) { down(connection); return; }
        auto current = RpcTlsSession::create(connection, serverContext, {
            .closed = [&](auto error) { GAMENET_TEST_ASSERT(error == TlsError::Graceful); ++serverClosed; }
        }, [](const auto& request, auto reply) {
            GAMENET_TEST_ASSERT(reply({RpcStatus::Ok, request.payload}) == RpcOperation::Accepted);
        }, options);
        connection->setContext(current); (void)current->start();
    });
    server.start();
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "rpc-tls-client");
    wire(client);
    std::weak_ptr<RpcTlsSession> weak;
    std::shared_ptr<RpcChannel> channel;
    client.setConnectionCallback([&](const auto& connection) {
        if (!connection->connected()) { down(connection); return; }
        auto clientOptions = options; clientOptions.peerName = "localhost";
        auto current = RpcTlsSession::create(connection, clientContext, {
            .ready = [&, weakConnection = std::weak_ptr(connection)] {
                ++ready;
                GAMENET_TEST_ASSERT(channel->call(1, payload, 3s, [&, weakConnection](auto result) {
                    GAMENET_TEST_ASSERT(result.status == RpcStatus::Ok && result.payload == payload); ++completed;
                    if (auto c = weakConnection.lock()) (void)session(c)->shutdown();
                }).status == RpcOperation::Accepted);
            },
            .closed = [&](auto error) {
                GAMENET_TEST_ASSERT(error == TlsError::Graceful && completed == 1 && channel->snapshot().pendingRequests == 0);
                ++clientClosed;
            }
        }, {}, clientOptions);
        weak = current; channel = current->channel(); connection->setContext(current); (void)current->start();
    });
    loop.runEvery(1ms, [&] { if (clientClosed && serverClosed) { client.stop(); server.stop(); loop.quit(); } });
    loop.runAfter(10s, [] { GAMENET_TEST_FAIL("real TCP RPC/TLS echo timed out"); });
    client.connect(); loop.loop();
    GAMENET_TEST_ASSERT(ready == 1 && completed == 1 && clientClosed == 1 && serverClosed == 1 && weak.expired());
}
void independentBatchedPeer(gamenet::test::TlsCertificates& cert) {
    gamenet::net::EventLoop loop;
    constexpr unsigned requests = 48;
    unsigned replies = 0, closed = 0;
    std::atomic<bool> peerDone{false};
    std::shared_ptr<RpcChannel> channel;
    auto context = TlsContext::server(cert.certificate.string(), cert.key.string());
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "batched-openssl-rpc");
    wire(server);
    server.setConnectionCallback([&](const auto& connection) {
        if (!connection->connected()) { down(connection); return; }
        RpcTlsOptions options; options.rpc.maxFramesPerDispatch = 1;
        options.tls.bioCapacity = 1024; options.tls.maxStepsPerDrive = 2;
        auto current = RpcTlsSession::create(connection, context, {
            .ready = [&] {
                for (unsigned i = 0; i < requests; ++i)
                    GAMENET_TEST_ASSERT(channel->call(7, "batch-" + std::to_string(i), 5s, [&, i](auto result) {
                        GAMENET_TEST_ASSERT(result.status == RpcStatus::Ok && result.payload == "batch-" + std::to_string(i)); ++replies;
                    }).status == RpcOperation::Accepted);
            },
            .closed = [&](auto error) {
                const auto s = channel->snapshot();
                GAMENET_TEST_ASSERT(error == TlsError::Graceful && replies == requests && s.closed &&
                    s.pendingRequests == 0 && s.pendingBytes == 0 && s.inboundRequests == 0 &&
                    !s.dispatching && !s.continuationQueued && s.acceptedCalls == s.completedCalls);
                ++closed;
            }
        }, {}, options);
        channel = current->channel(); connection->setContext(current); (void)current->start();
    });
    server.start(); const auto address = server.listenAddress();
    std::thread peer([&] {
        const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        GAMENET_TEST_ASSERT(gamenet::net::sockets::isValid(socket));
#ifdef _WIN32
        const DWORD timeout = 5000;
        GAMENET_TEST_ASSERT(::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0);
        GAMENET_TEST_ASSERT(::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0);
#else
        const timeval timeout{5, 0};
        GAMENET_TEST_ASSERT(::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        GAMENET_TEST_ASSERT(::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
#endif
        GAMENET_TEST_ASSERT(::connect(socket, address.getSockAddr(), address.getSockAddrLen()) == 0);
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> sslContext(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
        GAMENET_TEST_ASSERT(sslContext && SSL_CTX_set_min_proto_version(sslContext.get(), TLS1_2_VERSION) == 1);
        GAMENET_TEST_ASSERT(SSL_CTX_set_max_proto_version(sslContext.get(), TLS1_2_VERSION) == 1);
        SSL_CTX_set_verify(sslContext.get(), SSL_VERIFY_PEER, nullptr);
        GAMENET_TEST_ASSERT(SSL_CTX_load_verify_locations(sslContext.get(), cert.ca.string().c_str(), nullptr) == 1);
        std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(sslContext.get()), SSL_free);
        GAMENET_TEST_ASSERT(ssl && SSL_set_fd(ssl.get(), static_cast<int>(socket)) == 1 &&
            SSL_set1_host(ssl.get(), "localhost") == 1 && SSL_set_tlsext_host_name(ssl.get(), "localhost") == 1);
        GAMENET_TEST_ASSERT(SSL_connect(ssl.get()) == 1 && SSL_get_verify_result(ssl.get()) == X509_V_OK);
        gamenet::protocol::PacketFramer framer;
        std::string batch; unsigned received = 0;
        while (received < requests) {
            char bytes[16384]; std::size_t size{};
            GAMENET_TEST_ASSERT(SSL_read_ex(ssl.get(), bytes, sizeof(bytes), &size) == 1);
            auto frames = framer.push(std::string_view(bytes, size));
            GAMENET_TEST_ASSERT(!frames.needsContinuation && !framer.faulted());
            for (const auto& frame : frames.frames) {
                const auto request = RpcCodec::decode(frame);
                GAMENET_TEST_ASSERT(request && request->kind == RpcMessageKind::Request);
                batch += *framer.encode(*RpcCodec::encode({RpcMessageKind::Response, request->requestId, 0, 0, request->payload}));
                ++received;
            }
        }
        // One TLS record holds many RPC frames, then close_notify follows.
        // The server's RPC budget=1 must retire all continuations before closed.
        std::size_t written{};
        GAMENET_TEST_ASSERT(SSL_write_ex(ssl.get(), batch.data(), batch.size(), &written) == 1 && written == batch.size());
        int result = SSL_shutdown(ssl.get()); if (result == 0) result = SSL_shutdown(ssl.get());
        GAMENET_TEST_ASSERT(result == 1);
        ssl.reset(); gamenet::net::sockets::close(socket);
        peerDone.store(true, std::memory_order_release);
    });
    loop.runEvery(1ms, [&] { if (peerDone.load(std::memory_order_acquire) && closed) { server.stop(); loop.quit(); } });
    loop.runAfter(10s, [] { GAMENET_TEST_FAIL("batched independent RPC/TLS peer timed out"); });
    loop.loop(); peer.join();
    GAMENET_TEST_ASSERT(replies == requests && closed == 1);
}
}  // namespace
int main() {
    gamenet::test::TlsCertificates cert;
    std::fprintf(stderr, "RPC-C2 integration: session echo\n"); sessionEcho(cert);
    std::fprintf(stderr, "RPC-C2 integration: independent batched EOF\n"); independentBatchedPeer(cert);
    const auto clientContext = TlsContext::client(cert.ca.string());
    const auto serverContext = TlsContext::server(cert.certificate.string(), cert.key.string());
    gamenet::test::runRpcReconnectStress([&](const gamenet::net::TcpConnectionPtr& connection,
        RpcChannel::Handler handler, std::function<void()> ready, std::function<void()> closed, bool client) {
        auto result = std::make_shared<gamenet::test::RpcStressSession>();
        RpcTlsOptions options; options.peerName = client ? "localhost" : "";
        options.tls.bioCapacity = 1024; options.tls.maxStepsPerDrive = 16;
        if (client) { options.tls.maxPendingWrites = 2; options.tls.maxPendingPlaintextBytes = 16384; }
        auto current = RpcTlsSession::create(connection, client ? clientContext : serverContext, {
            .ready = std::move(ready),
            .closed = [closed = std::move(closed)](auto reason) {
                GAMENET_TEST_ASSERT(reason != TlsError::Graceful); closed();
            }
        }, std::move(handler), options);
        result->channel = current->channel(); result->lifetime = current;
        result->start = [current] { (void)current->start(); };
        result->down = [current] { current->onDisconnect(); };
        result->writable = [current] { current->onWritable(); };
        result->feed = [current](auto& input) { (void)current->onMessage(input); };
        result->settled = [current] {
            const auto s = current->snapshot().tls;
            GAMENET_TEST_ASSERT(!s.pendingPlaintextBytes && !s.pendingWrites &&
                s.acceptedPlaintextBytes == s.encryptedPlaintextBytes + s.discardedPlaintextBytes);
        };
        return result;
    }, "RPC-C3 TLS");
}
