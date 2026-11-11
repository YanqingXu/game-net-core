// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/rpc_tls/RpcTlsSession.h>
#include <stdexcept>
int main() {
    try { (void)gamenet::rpc::RpcTlsSession::create({}, {}, {}); }
    catch (const std::invalid_argument&) { return 0; }
    return 1;
}
