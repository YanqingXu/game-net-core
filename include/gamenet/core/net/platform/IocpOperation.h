#pragma once

#include "gamenet/core/net/SocketTypes.h"

#ifdef _WIN32

#include <winsock2.h>
#include <windows.h>

namespace gamenet::net {

class Channel;

enum class IocpOperationKind {
    Accept,
    Connect,
    Read,
    Write,
};

struct IocpOperation {
    OVERLAPPED overlapped{};
    IocpOperationKind kind;
    Channel* channel{nullptr};
    DWORD bytesTransferred{0};
    DWORD error{0};
};

}  // namespace gamenet::net

#endif
