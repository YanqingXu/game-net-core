#include "gamenet/core/net/InetAddress.h"

#include "support/TestAssert.h"
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

int main() {
    {
        gamenet::net::InetAddress addr("2001:db8::2", 3000);
        GAMENET_TEST_ASSERT(addr.isIpv6());
        GAMENET_TEST_ASSERT(addr.getSockAddr()->sa_family == AF_INET6);
        GAMENET_TEST_ASSERT(addr.getSockAddrLen() == static_cast<socklen_t>(sizeof(sockaddr_in6)));

        sockaddr_storage storage{};
        std::memcpy(&storage, addr.getSockAddr(), addr.getSockAddrLen());

        gamenet::net::InetAddress roundTrip(storage);
        GAMENET_TEST_ASSERT(roundTrip.isIpv6());
        GAMENET_TEST_ASSERT(roundTrip.toIp() == "2001:db8::2");
        GAMENET_TEST_ASSERT(roundTrip.toIpPort() == "[2001:db8::2]:3000");
        GAMENET_TEST_ASSERT(roundTrip.port() == 3000);
    }

    {
        sockaddr_in6 raw{};
        raw.sin6_family = AF_INET6;
        raw.sin6_port = htons(7000);
        const int converted = ::inet_pton(AF_INET6, "::1", &raw.sin6_addr);
        GAMENET_TEST_ASSERT(converted == 1);

        gamenet::net::InetAddress addr(0, false);
        GAMENET_TEST_ASSERT(addr.isIpv4());

        addr.setSockAddrInet6(raw);
        GAMENET_TEST_ASSERT(addr.isIpv6());
        GAMENET_TEST_ASSERT(addr.family() == AF_INET6);
        GAMENET_TEST_ASSERT(addr.toIp() == "::1");
        GAMENET_TEST_ASSERT(addr.toIpPort() == "[::1]:7000");
    }

    {
        bool threw = false;
        try {
            gamenet::net::InetAddress("999.999.999.999", 80);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        GAMENET_TEST_ASSERT(threw);
    }

    return 0;
}
