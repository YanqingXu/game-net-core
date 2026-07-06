#include <gamenet/core/net/Buffer.h>
#include <gamenet/core/net/InetAddress.h>

int main() {
    gamenet::net::Buffer buffer;
    buffer.append("ok", 2);

    gamenet::net::InetAddress address(0);
    if (buffer.readableBytes() != 2 || address.port() != 0) {
        return 1;
    }

    return 0;
}
