#pragma once

// Structured semantic close result shared by TcpConnection, TcpServer, and
// TcpClient observers. Native errors are diagnostics; callers should branch on
// the stable semantic reason.

namespace gamenet::net {

enum class TcpConnectionCloseReason {
    PeerEof,
    Reset,
    ConnectTimeout,
    InputLimit,
    OutputOverload,
    AdmissionPolicy,
    GracefulShutdown,
    ForcedShutdown,
    CallbackFailure,
    InternalError,
};

struct TcpConnectionCloseInfo {
    TcpConnectionCloseReason reason{TcpConnectionCloseReason::InternalError};
    int nativeError{0};

    bool operator==(const TcpConnectionCloseInfo&) const = default;
};

enum class TcpConnectionClosePhase {
    Open,
    Closing,
    SocketClosed,
    CompletionDraining,
    Closed,
};

}  // namespace gamenet::net
