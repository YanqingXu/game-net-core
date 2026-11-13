// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Independent executable: unchanged upstream, native sockets, no GameNet headers/libraries.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Fd = SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using Fd = int;
#endif
#include "../../../third_party/kcp/ikcp.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
struct Wire {
    Fd fd;
    sockaddr_storage remote;
    int size;
};
void require(bool v) {
    if (!v) {
        std::cerr << "upstream peer contract failed\n";
        std::abort();
    }
}
int main(int argc, char** argv) {
    require(argc == 4);
#ifdef _WIN32
    WSADATA data;
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    const bool v6 = std::string(argv[1]) == "6";
    Wire wire{};
    wire.fd = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_storage local{};
    if (v6) {
        auto& a = reinterpret_cast<sockaddr_in6&>(local);
        a.sin6_family = AF_INET6;
        inet_pton(AF_INET6, "::1", &a.sin6_addr);
        wire.size = sizeof(a);
    } else {
        auto& a = reinterpret_cast<sockaddr_in&>(local);
        a.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        wire.size = sizeof(a);
    }
    require(bind(wire.fd, reinterpret_cast<sockaddr*>(&local), wire.size) == 0);
    wire.remote = local;
    const auto port = htons(static_cast<unsigned short>(std::stoi(argv[2])));
    if (v6)
        reinterpret_cast<sockaddr_in6&>(wire.remote).sin6_port = port;
    else
        reinterpret_cast<sockaddr_in&>(wire.remote).sin_port = port;
#ifdef _WIN32
    u_long nonblocking = 1;
    require(ioctlsocket(wire.fd, FIONBIO, &nonblocking) == 0);
    int size = sizeof(local);
#else
    require(fcntl(wire.fd, F_SETFL, O_NONBLOCK) == 0);
    socklen_t size = sizeof(local);
#endif
    require(getsockname(wire.fd, reinterpret_cast<sockaddr*>(&local), &size) == 0);
    std::cout << "PORT "
              << ntohs(v6 ? reinterpret_cast<sockaddr_in6&>(local).sin6_port
                          : reinterpret_cast<sockaddr_in&>(local).sin_port)
              << std::endl;
    auto* kcp = ikcp_create(static_cast<IUINT32>(std::stoul(argv[3])), &wire);
    require(kcp);
    require(ikcp_setmtu(kcp, 1200) == 0);
    ikcp_wndsize(kcp, 32, 128);
    ikcp_nodelay(kcp, 0, 20, 2, 0);
    ikcp_setoutput(kcp, [](const char* p, int n, ikcpcb*, void* ctx) {
        auto& w = *static_cast<Wire*>(ctx);
        return sendto(w.fd, p, n, 0, reinterpret_cast<sockaddr*>(&w.remote), w.size) == n ? 0 : -1;
    });
    const std::vector<std::string> messages = {"", std::string("binary\0\xff", 8),
                                               std::string(14000, 'z'), "tail"};
    for (const auto& message : messages)
        require(ikcp_send(kcp, message.data(), static_cast<int>(message.size())) ==
                static_cast<int>(message.size()));
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto complete = start;
    bool completing = false;
    std::size_t received = 0;
    while (Clock::now() - start < std::chrono::seconds(20)) {
        const auto now = Clock::now();
        ikcp_update(kcp, static_cast<IUINT32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  now.time_since_epoch())
                                                  .count()));
        char bytes[20000];
        for (int i = 0; i < 64; ++i) {
            const auto n = recv(wire.fd, bytes, sizeof(bytes), 0);
            if (n < 0)
                break;
            require(ikcp_input(kcp, bytes, n) == 0);
        }
        while (ikcp_peeksize(kcp) >= 0) {
            const auto n = ikcp_recv(kcp, bytes, sizeof(bytes));
            require(n >= 0 && received < messages.size() &&
                    std::string(bytes, n) == messages[received]);
            ++received;
        }
        if (received == messages.size() && !ikcp_waitsnd(kcp)) {
            if (!completing) {
                complete = now;
                completing = true;
            }
            if (now - complete > std::chrono::milliseconds(500))
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(received == 4 && !ikcp_waitsnd(kcp));
    ikcp_release(kcp);
#ifdef _WIN32
    closesocket(wire.fd);
    WSACleanup();
#else
    close(wire.fd);
#endif
    std::cout << "{\"implementation\":\"unmodified-upstream\",\"status\":\"passed\",\"messages\":4}"
              << std::endl;
}
