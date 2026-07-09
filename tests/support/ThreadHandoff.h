#pragma once

#include <chrono>
#include <thread>
#include <utility>

namespace gamenet::test {

class NonOwnerThreadHandoff {
public:
    NonOwnerThreadHandoff() = default;

    explicit NonOwnerThreadHandoff(std::thread handoff)
        : handoff_(std::move(handoff)) {}

    NonOwnerThreadHandoff(const NonOwnerThreadHandoff&) = delete;
    NonOwnerThreadHandoff& operator=(const NonOwnerThreadHandoff&) = delete;

    NonOwnerThreadHandoff(NonOwnerThreadHandoff&&) noexcept = default;

    NonOwnerThreadHandoff& operator=(NonOwnerThreadHandoff&& other) noexcept {
        if (this != &other) {
            join();
            handoff_ = std::move(other.handoff_);
        }
        return *this;
    }

    ~NonOwnerThreadHandoff() {
        join();
    }

    bool joinable() const noexcept {
        return handoff_.joinable();
    }

    void join() {
        if (handoff_.joinable()) {
            handoff_.join();
        }
    }

private:
    std::thread handoff_;
};

template <typename Function>
NonOwnerThreadHandoff startNonOwnerThread(Function&& function) {
    return NonOwnerThreadHandoff(std::thread(std::forward<Function>(function)));
}

template <typename Rep, typename Period, typename Function>
NonOwnerThreadHandoff startNonOwnerThreadAfter(
    std::chrono::duration<Rep, Period> delay,
    Function&& function) {
    return startNonOwnerThread(
        [delay, function = std::forward<Function>(function)]() mutable {
            std::this_thread::sleep_for(delay);
            function();
        });
}

template <typename Function>
void runFromNonOwnerThread(Function&& function) {
    auto handoff = startNonOwnerThread(std::forward<Function>(function));
    handoff.join();
}

}  // namespace gamenet::test
