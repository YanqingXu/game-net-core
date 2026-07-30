#include "gamenet/core/net/TcpOutputMemoryBudget.h"

#include "support/TestAssert.h"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace gamenet::net::detail {

class TcpOutputMemoryBudgetHarness {
public:
    static bool reserve(
        TcpOutputMemoryBudget& budget,
        std::size_t bytes) noexcept {
        return budget.tryReserve(bytes);
    }

    static void release(
        TcpOutputMemoryBudget& budget,
        std::size_t bytes) noexcept {
        budget.release(bytes);
    }
};

}  // namespace gamenet::net::detail

int main() {
    using Budget = gamenet::net::TcpOutputMemoryBudget;
    using Options = gamenet::net::TcpOutputMemoryBudgetOptions;
    using Harness =
        gamenet::net::detail::TcpOutputMemoryBudgetHarness;

    bool rejectedZeroLimit = false;
    try {
        Budget invalid(Options{
            .hardLimitBytes = 0,
            .recoveryThresholdBytes = 0,
        });
    } catch (const std::invalid_argument&) {
        rejectedZeroLimit = true;
    }
    GAMENET_TEST_ASSERT(rejectedZeroLimit);

    bool rejectedMissingHysteresis = false;
    try {
        Budget invalid(Options{
            .hardLimitBytes = 10,
            .recoveryThresholdBytes = 10,
        });
    } catch (const std::invalid_argument&) {
        rejectedMissingHysteresis = true;
    }
    GAMENET_TEST_ASSERT(rejectedMissingHysteresis);

    Budget budget(Options{
        .hardLimitBytes = 10,
        .recoveryThresholdBytes = 4,
    });
    GAMENET_TEST_ASSERT(Harness::reserve(budget, 6));
    GAMENET_TEST_ASSERT(!Harness::reserve(budget, 5));
    auto snapshot = budget.snapshot();
    GAMENET_TEST_ASSERT(snapshot.pendingBytes == 6);
    GAMENET_TEST_ASSERT(snapshot.peakPendingBytes == 6);
    GAMENET_TEST_ASSERT(snapshot.rejectedReservations == 1);
    GAMENET_TEST_ASSERT(snapshot.overloaded);

    // Capacity is available for one byte, but the hysteretic gate stays closed
    // until pending reaches the explicit recovery threshold.
    GAMENET_TEST_ASSERT(!Harness::reserve(budget, 1));
    Harness::release(budget, 2);
    GAMENET_TEST_ASSERT(Harness::reserve(budget, 1));
    Harness::release(budget, 5);
    snapshot = budget.snapshot();
    GAMENET_TEST_ASSERT(snapshot.pendingBytes == 0);
    GAMENET_TEST_ASSERT(!snapshot.overloaded);

    // An oversized request against an idle budget is rejected without
    // latching out a following small request.
    GAMENET_TEST_ASSERT(!Harness::reserve(budget, 11));
    GAMENET_TEST_ASSERT(!budget.snapshot().overloaded);
    GAMENET_TEST_ASSERT(Harness::reserve(budget, 1));
    Harness::release(budget, 1);

    constexpr std::size_t threadCount = 32;
    constexpr std::size_t bytesPerReservation = 16;
    Budget concurrentBudget(Options{
        .hardLimitBytes = 128,
        .recoveryThresholdBytes = 64,
    });
    std::barrier startGate(
        static_cast<std::ptrdiff_t>(threadCount + 1));
    std::barrier attemptedGate(
        static_cast<std::ptrdiff_t>(threadCount + 1));
    std::barrier releaseGate(
        static_cast<std::ptrdiff_t>(threadCount + 1));
    std::atomic<std::size_t> accepted{0};
    std::vector<std::jthread> workers;
    workers.reserve(threadCount);

    for (std::size_t index = 0; index < threadCount; ++index) {
        workers.emplace_back([&] {
            startGate.arrive_and_wait();
            const bool reserved = Harness::reserve(
                concurrentBudget,
                bytesPerReservation);
            if (reserved) {
                accepted.fetch_add(1, std::memory_order_relaxed);
            }
            attemptedGate.arrive_and_wait();
            releaseGate.arrive_and_wait();
            if (reserved) {
                Harness::release(
                    concurrentBudget,
                    bytesPerReservation);
            }
        });
    }

    startGate.arrive_and_wait();
    attemptedGate.arrive_and_wait();
    const auto saturated = concurrentBudget.snapshot();
    GAMENET_TEST_ASSERT(accepted.load(std::memory_order_relaxed) == 8);
    GAMENET_TEST_ASSERT(saturated.pendingBytes == 128);
    GAMENET_TEST_ASSERT(saturated.peakPendingBytes == 128);
    GAMENET_TEST_ASSERT(saturated.rejectedReservations == 24);
    GAMENET_TEST_ASSERT(saturated.overloaded);
    releaseGate.arrive_and_wait();
    workers.clear();

    const auto released = concurrentBudget.snapshot();
    GAMENET_TEST_ASSERT(released.pendingBytes == 0);
    GAMENET_TEST_ASSERT(released.peakPendingBytes == 128);
    GAMENET_TEST_ASSERT(!released.overloaded);

    return 0;
}
