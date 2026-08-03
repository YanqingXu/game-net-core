#pragma once

// Source-private deterministic seam for the accepted-fd construction
// transaction. Installed callers cannot inject constructor failures.

namespace gamenet::net::detail {

void injectNextTcpConnectionConstructionFailureForTesting() noexcept;
bool consumeTcpConnectionConstructionFailureForTesting(
    bool connectionSocketOwnsFd) noexcept;
bool tcpConnectionConstructionFailureWasConsumedForTesting() noexcept;
bool tcpConnectionConstructionFailureObservedSocketOwnerForTesting() noexcept;

class TcpConnectionConstructionHarness final {
public:
    TcpConnectionConstructionHarness() = delete;

    static void failNextBeforeSocketClaim() noexcept {
        injectNextTcpConnectionConstructionFailureForTesting();
    }

    static bool failureWasConsumed() noexcept {
        return tcpConnectionConstructionFailureWasConsumedForTesting();
    }

    static bool failureObservedSocketOwner() noexcept {
        return tcpConnectionConstructionFailureObservedSocketOwnerForTesting();
    }
};

}  // namespace gamenet::net::detail
