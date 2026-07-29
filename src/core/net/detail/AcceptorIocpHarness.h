#pragma once

#include <cstddef>
#include <cstdint>

namespace gamenet::net::detail {

struct IocpAcceptPoolObservations {
    std::size_t currentSlots{0};
    std::size_t peakSlots{0};
    std::size_t currentSlotSockets{0};
    std::size_t peakSlotSockets{0};
    std::size_t currentSubmitted{0};
    std::size_t peakSubmitted{0};
    std::uint64_t submissions{0};
    std::uint64_t completions{0};
    std::uint64_t cancellationRequests{0};
    std::uint64_t maxGeneration{0};
};

void resetIocpAcceptPoolObservationsForTesting() noexcept;
IocpAcceptPoolObservations iocpAcceptPoolObservationsForTesting() noexcept;

// The next failure is injected after the requested number of successful
// AcceptEx calls made by the current process. It is a synchronous non-pending
// failure and therefore creates no Poller lease or completion obligation.
void injectIocpAcceptSubmissionErrorForTesting(
    std::size_t successfulSubmissionsBeforeFailure,
    int error) noexcept;

}  // namespace gamenet::net::detail
