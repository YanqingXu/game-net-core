#pragma once

#include "gamenet/core/base/Timestamp.h"
#include "gamenet/core/net/SocketTypes.h"

#ifdef _WIN32

#include <cstdint>
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

using IocpTerminalObserver =
    void (*)(void* context, IocpOperationKind kind) noexcept;
using IocpCompletionConsumer =
    void (*)(
        void* context,
        gamenet::base::Timestamp observedAt,
        bool observerCurrent);

struct IocpOperation {
    OVERLAPPED overlapped{};
    IocpOperationKind kind;
    Channel* channel{nullptr};
    DWORD bytesTransferred{0};
    DWORD error{0};
    std::uint64_t generation{0};
    std::uint64_t terminalGeneration{0};
    SocketFd observerSource{kInvalidSocket};
    std::uint64_t observerRegistrationGeneration{0};
    void* terminalContext{nullptr};
    IocpTerminalObserver terminalObserver{nullptr};
    void* completionContext{nullptr};
    IocpCompletionConsumer completionConsumer{nullptr};
    bool submissionPrepared{false};
    bool submissionAccepted{false};
    bool observerIdentityCaptured{false};
    bool shutdownObligation{false};
    bool completionObserved{false};
    IocpOperation* nextPublishedCompletion{nullptr};
};

}  // namespace gamenet::net

#endif
