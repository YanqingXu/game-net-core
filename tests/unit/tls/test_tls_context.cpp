// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/tls/TlsContext.h"
#include "support/TlsCertificates.h"
#include "support/TestAssert.h"
#include <stdexcept>
#include <thread>
int main() {
    gamenet::test::TlsCertificates cert;
    const auto client = gamenet::tls::TlsContext::client(cert.ca.string());
    const auto server = gamenet::tls::TlsContext::server(cert.certificate.string(), cert.key.string());
    GAMENET_TEST_ASSERT(client->role() == gamenet::tls::TlsContext::Role::Client);
    GAMENET_TEST_ASSERT(server->role() == gamenet::tls::TlsContext::Role::Server);
    unsigned rejected = 0;
    try { (void)gamenet::tls::TlsContext::client((cert.directory / "missing").string()); }
    catch (const std::runtime_error&) { ++rejected; }
    try { (void)gamenet::tls::TlsContext::server(cert.certificate.string(), cert.unrelatedKey.string()); }
    catch (const std::runtime_error&) { ++rejected; }
    try { (void)gamenet::tls::TlsContext::server("", ""); }
    catch (const std::runtime_error&) { ++rejected; }
    std::thread observer([client, server] {
        GAMENET_TEST_ASSERT(client->role() != server->role());
    }); observer.join();
    GAMENET_TEST_ASSERT(rejected == 3);
}
