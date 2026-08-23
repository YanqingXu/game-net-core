// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include <gamenet/core/base/Timestamp.h>
#include <gamenet/core/net/Buffer.h>
#include <gamenet/core/net/InetAddress.h>

#include <string>

int main() {
    gamenet::net::Buffer buffer;
    buffer.append("upgrade", 7);
    const gamenet::net::InetAddress loopback(24680, true);
    const auto timestamp = gamenet::base::now();

    if (buffer.retrieveAllAsString() != "upgrade" ||
        loopback.toIpPort() != "127.0.0.1:24680" ||
        timestamp.time_since_epoch().count() <= 0) {
        return 1;
    }
    return 0;
}
