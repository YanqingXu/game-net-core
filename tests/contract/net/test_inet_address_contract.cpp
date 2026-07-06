#include "gamenet/core/net/InetAddress.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

int main() {
    {
        gamenet::net::InetAddress addr("2001:db8::2", 3000);
        assert(addr.isIpv6());
        assert(addr.getSockAddr()->sa_family == AF_INET6);
        assert(addr.getSockAddrLen() == static_cast<socklen_t>(sizeof(sockaddr_in6)));

        sockaddr_storage storage{};
        std::memcpy(&storage, addr.getSockAddr(), addr.getSockAddrLen());

        gamenet::net::InetAddress roundTrip(storage);
        assert(roundTrip.isIpv6());
        assert(roundTrip.toIp() == "2001:db8::2");
        assert(roundTrip.toIpPort() == "[2001:db8::2]:3000");
        assert(roundTrip.port() == 3000);
    }

    {
        sockaddr_in6 raw{};
        raw.sin6_family = AF_INET6;
        raw.sin6_port = htons(7000);
        const int converted = ::inet_pton(AF_INET6, "::1", &raw.sin6_addr);
        assert(converted == 1);

        gamenet::net::InetAddress addr(0, false);
        assert(addr.isIpv4());

        addr.setSockAddrInet6(raw);
        assert(addr.isIpv6());
        assert(addr.family() == AF_INET6);
        assert(addr.toIp() == "::1");
        assert(addr.toIpPort() == "[::1]:7000");
    }

    {
        bool threw = false;
        try {
            gamenet::net::InetAddress("999.999.999.999", 80);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    return 0;
}
