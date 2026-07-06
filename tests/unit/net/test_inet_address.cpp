#include "gamenet/core/net/InetAddress.h"

#include "support/TestAssert.h"
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

int main() {
    {
        gamenet::net::InetAddress addr(8080, false);
        GAMENET_TEST_ASSERT(addr.isIpv4());
        GAMENET_TEST_ASSERT(!addr.isIpv6());
        GAMENET_TEST_ASSERT(addr.family() == AF_INET);
        GAMENET_TEST_ASSERT(addr.port() == 8080);
        GAMENET_TEST_ASSERT(addr.toIp() == "0.0.0.0");
        GAMENET_TEST_ASSERT(addr.toIpPort() == "0.0.0.0:8080");
    }

    {
        gamenet::net::InetAddress addr(9090, true);
        GAMENET_TEST_ASSERT(addr.isIpv4());
        GAMENET_TEST_ASSERT(addr.toIp() == "127.0.0.1");
        GAMENET_TEST_ASSERT(addr.port() == 9090);
    }

    {
        gamenet::net::InetAddress addr("192.168.1.1", 443);
        GAMENET_TEST_ASSERT(addr.isIpv4());
        GAMENET_TEST_ASSERT(addr.toIp() == "192.168.1.1");
        GAMENET_TEST_ASSERT(addr.toIpPort() == "192.168.1.1:443");
    }

    {
        gamenet::net::InetAddress addr("::1", 8080);
        GAMENET_TEST_ASSERT(addr.isIpv6());
        GAMENET_TEST_ASSERT(!addr.isIpv4());
        GAMENET_TEST_ASSERT(addr.family() == AF_INET6);
        GAMENET_TEST_ASSERT(addr.toIp() == "::1");
        GAMENET_TEST_ASSERT(addr.toIpPort() == "[::1]:8080");
    }

    {
        gamenet::net::InetAddress addr("[::1]", 9090);
        GAMENET_TEST_ASSERT(addr.isIpv6());
        GAMENET_TEST_ASSERT(addr.toIp() == "::1");
        GAMENET_TEST_ASSERT(addr.port() == 9090);
    }

    {
        gamenet::net::InetAddress addr("10.0.0.1", 4000);
        sockaddr_storage storage{};
        std::memcpy(&storage, addr.getSockAddr(), addr.getSockAddrLen());

        gamenet::net::InetAddress roundTrip(storage);
        GAMENET_TEST_ASSERT(roundTrip.isIpv4());
        GAMENET_TEST_ASSERT(roundTrip.toIp() == "10.0.0.1");
        GAMENET_TEST_ASSERT(roundTrip.port() == 4000);
    }

    {
        bool threw = false;
        try {
            gamenet::net::InetAddress("not-an-ip", 80);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        GAMENET_TEST_ASSERT(threw);
    }

    return 0;
}
