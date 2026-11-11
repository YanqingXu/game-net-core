// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/tls/TlsStream.h>
#include <gamenet/core/net/EventLoop.h>
#include <stdexcept>
int main() {
    gamenet::net::EventLoop loop;
    unsigned rejected = 0;
    try { (void)gamenet::tls::TlsContext::server("", ""); }
    catch (const std::runtime_error&) { ++rejected; }
    try { (void)gamenet::tls::TlsStream::create({}, {}, {}); }
    catch (const std::invalid_argument&) { ++rejected; }
    return rejected == 2 ? 0 : 1;
}
