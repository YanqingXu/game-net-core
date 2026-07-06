#include "gamenet/core/net/InetAddress.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

int main() {
    {
        gamenet::net::InetAddress addr(8080, false);
        assert(addr.isIpv4());
        assert(!addr.isIpv6());
        assert(addr.family() == AF_INET);
        assert(addr.port() == 8080);
        assert(addr.toIp() == "0.0.0.0");
        assert(addr.toIpPort() == "0.0.0.0:8080");
    }

    {
        gamenet::net::InetAddress addr(9090, true);
        assert(addr.isIpv4());
        assert(addr.toIp() == "127.0.0.1");
        assert(addr.port() == 9090);
    }

    {
        gamenet::net::InetAddress addr("192.168.1.1", 443);
        assert(addr.isIpv4());
        assert(addr.toIp() == "192.168.1.1");
        assert(addr.toIpPort() == "192.168.1.1:443");
    }

    {
        gamenet::net::InetAddress addr("::1", 8080);
        assert(addr.isIpv6());
        assert(!addr.isIpv4());
        assert(addr.family() == AF_INET6);
        assert(addr.toIp() == "::1");
        assert(addr.toIpPort() == "[::1]:8080");
    }

    {
        gamenet::net::InetAddress addr("[::1]", 9090);
        assert(addr.isIpv6());
        assert(addr.toIp() == "::1");
        assert(addr.port() == 9090);
    }

    {
        gamenet::net::InetAddress addr("10.0.0.1", 4000);
        sockaddr_storage storage{};
        std::memcpy(&storage, addr.getSockAddr(), addr.getSockAddrLen());

        gamenet::net::InetAddress roundTrip(storage);
        assert(roundTrip.isIpv4());
        assert(roundTrip.toIp() == "10.0.0.1");
        assert(roundTrip.port() == 4000);
    }

    {
        bool threw = false;
        try {
            gamenet::net::InetAddress("not-an-ip", 80);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    return 0;
}
