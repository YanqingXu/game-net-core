// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/tls/TlsStream.h"
#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/TcpClient.h"
#include "gamenet/core/net/TcpConnection.h"
#include "gamenet/core/net/TcpServer.h"
#include "gamenet/core/net/SocketsOps.h"
#include "support/TlsCertificates.h"
#include "support/TestAssert.h"
#include <openssl/ssl.h>
#include <any>
#include <atomic>
#include <thread>
#include <cstdio>

using namespace gamenet::tls;
using namespace std::chrono_literals;
namespace {
std::shared_ptr<TlsStream> stream(const gamenet::net::TcpConnectionPtr& connection) {
    const auto* value = std::any_cast<std::shared_ptr<TlsStream>>(&connection->getContext());
    return value ? *value : nullptr;
}
void configure(gamenet::net::TcpServer& server, std::shared_ptr<TlsContext> context,
    unsigned& ready, unsigned& closed, std::string& received) {
    server.setConnectionCallback([context, &ready, &closed, &received](const auto& connection) {
        if (!connection->connected()) {
            if (auto tls = stream(connection)) tls->onDisconnect();
            connection->setContext(std::any{}); return;
        }
        const std::weak_ptr<gamenet::net::TcpConnection> weak = connection;
        TlsOptions options; options.bioCapacity = 1024; options.maxStepsPerDrive = 16;
        auto tls = TlsStream::create(connection, context, {
            .ready = [&ready] { ++ready; },
            .data = [weak, &ready, &received](auto bytes) {
                GAMENET_TEST_ASSERT(ready == 1);
                received.append(bytes);
                const auto current = weak.lock(); GAMENET_TEST_ASSERT(current);
                GAMENET_TEST_ASSERT(stream(current)->trySend(bytes) == TlsOperation::Accepted);
            },
            .closed = [&closed](auto error) { GAMENET_TEST_ASSERT(error == TlsError::Graceful); ++closed; }
        }, {}, options);
        connection->setContext(tls); GAMENET_TEST_ASSERT(tls->start() == TlsOperation::Accepted);
    });
    server.setMessageCallback([](const auto& connection, auto* input) {
        if (auto tls = stream(connection)) (void)tls->onMessage(*input); else input->retrieveAll();
    });
    server.setWriteCompleteCallback([](const auto& connection) {
        if (auto tls = stream(connection)) tls->onWritable();
    });
}
void streamClient(gamenet::test::TlsCertificates& cert) {
    gamenet::net::EventLoop loop;
    unsigned serverReady = 0, serverClosed = 0, clientReady = 0, clientClosed = 0;
    std::string sent = std::string("binary\0", 7) + std::string(65536, 'x'), serverData, clientData;
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "tls-echo");
    configure(server, TlsContext::server(cert.certificate.string(), cert.key.string()), serverReady, serverClosed, serverData);
    server.start();
    std::shared_ptr<TlsStream> clientTls;
    std::weak_ptr<TlsStream> clientWeak;
    TlsSnapshot clientFinal;
    const auto clientContext = TlsContext::client(cert.ca.string());
    gamenet::net::TcpClient client(&loop, server.listenAddress(), "tls-client");
    client.setConnectionCallback([&](const auto& connection) {
        if (!connection->connected()) {
            if (auto lease = clientTls) lease->onDisconnect();
            clientTls.reset(); // EOF continuation, not this application, retains TLS input
            return;
        }
        TlsOptions options; options.bioCapacity = 1024; options.maxStepsPerDrive = 16;
        clientTls = TlsStream::create(connection, clientContext, {
            .ready = [&] {
                ++clientReady;
                GAMENET_TEST_ASSERT(clientTls->trySend(sent) == TlsOperation::Accepted);
                (void)clientTls->shutdown();
            },
            .data = [&](auto bytes) { GAMENET_TEST_ASSERT(clientReady == 1); clientData.append(bytes); },
            .closed = [&](auto error) {
                if (error != TlsError::Graceful) std::fprintf(stderr,
                    "TLS client terminal=%d received=%zu expected=%zu\n", static_cast<int>(error), clientData.size(), sent.size());
                GAMENET_TEST_ASSERT(error == TlsError::Graceful); ++clientClosed;
                if (auto lease = clientWeak.lock()) clientFinal = lease->snapshot();
            }
        }, "localhost", options);
        clientWeak = clientTls;
        (void)clientTls->start();
    });
    client.setMessageCallback([&](const auto&, auto* input) { (void)clientTls->onMessage(*input); });
    client.setWriteCompleteCallback([&](const auto&) { if (clientTls) clientTls->onWritable(); });
    loop.runEvery(1ms, [&] {
        if (clientClosed && serverClosed) { client.stop(); server.stop(); loop.quit(); }
    });
    loop.runAfter(10s, [] { GAMENET_TEST_FAIL("real TCP TLS echo timed out"); });
    client.connect(); loop.loop();
    GAMENET_TEST_ASSERT(clientReady == 1 && serverReady == 1 && clientClosed == 1 && serverClosed == 1 &&
        clientData == sent && serverData == sent && clientFinal.discardedPlaintextBytes == 0 && clientWeak.expired());
}
void independentOpenSslPeer(gamenet::test::TlsCertificates& cert) {
    gamenet::net::EventLoop loop;
    unsigned ready = 0, closed = 0;
    std::string received;
    const std::string payload = std::string("independent\0peer", 16) + std::string(32768, 'i');
    std::atomic<bool> done{false};
    gamenet::net::TcpServer server(&loop, gamenet::net::InetAddress(0, true), "openssl-peer");
    configure(server, TlsContext::server(cert.certificate.string(), cert.key.string()), ready, closed, received);
    server.start();
    const auto address = server.listenAddress();
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
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
        GAMENET_TEST_ASSERT(context && SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) == 1);
        GAMENET_TEST_ASSERT(SSL_CTX_set_max_proto_version(context.get(), TLS1_2_VERSION) == 1);
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
        GAMENET_TEST_ASSERT(SSL_CTX_load_verify_locations(context.get(), cert.ca.string().c_str(), nullptr) == 1);
        std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context.get()), SSL_free);
        GAMENET_TEST_ASSERT(ssl && SSL_set_fd(ssl.get(), static_cast<int>(socket)) == 1 &&
            SSL_set1_host(ssl.get(), "localhost") == 1 && SSL_set_tlsext_host_name(ssl.get(), "localhost") == 1);
        GAMENET_TEST_ASSERT(SSL_connect(ssl.get()) == 1 && SSL_get_verify_result(ssl.get()) == X509_V_OK);
        std::size_t written{};
        GAMENET_TEST_ASSERT(SSL_write_ex(ssl.get(), payload.data(), payload.size(), &written) == 1 && written == payload.size());
        std::string echoed;
        while (echoed.size() < payload.size()) {
            char bytes[4096]; std::size_t count{};
            GAMENET_TEST_ASSERT(SSL_read_ex(ssl.get(), bytes, sizeof(bytes), &count) == 1);
            echoed.append(bytes, count);
        }
        GAMENET_TEST_ASSERT(echoed == payload);
        int result = SSL_shutdown(ssl.get()); if (result == 0) result = SSL_shutdown(ssl.get());
        GAMENET_TEST_ASSERT(result == 1);
        ssl.reset(); gamenet::net::sockets::close(socket);
        done.store(true, std::memory_order_release);
    });
    loop.runEvery(1ms, [&] { if (done.load(std::memory_order_acquire) && closed) { server.stop(); loop.quit(); } });
    loop.runAfter(10s, [] { GAMENET_TEST_FAIL("independent OpenSSL peer timed out"); });
    loop.loop(); peer.join();
    GAMENET_TEST_ASSERT(ready == 1 && closed == 1 && received == payload);
}
}  // namespace
int main() { gamenet::test::TlsCertificates cert; streamClient(cert); independentOpenSslPeer(cert); }
