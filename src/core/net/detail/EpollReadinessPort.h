#pragma once

#ifndef _WIN32

#include "ReadinessPort.h"
#include "gamenet/core/net/platform/Wakeup.h"

#include <sys/epoll.h>

#include <unordered_map>
#include <vector>

namespace gamenet::net {

class EventLoop;

namespace detail {

class EpollReadinessPortHarness;

class EpollReadinessPort final : public ReadinessPort {
public:
    explicit EpollReadinessPort(
        EventLoop* ownerLoop,
        ReadinessPortOptions options = {});
    ~EpollReadinessPort() override;

    ReadinessPortOptions options() const noexcept override;
    ReadinessRegistrationResult registerOrUpdate(
        ReadinessRegistrationRequest request) override;
    ReadinessPortResult cancel(Channel* target) override;
    bool has(const Channel* target) const override;
    std::optional<ReadinessRegistrationIdentity>
    registrationIdentity(const Channel* target) const override;
    bool isCurrent(
        ReadinessRegistrationIdentity identity,
        const Channel* target) const override;
    ReadinessWaitResult wait(int timeoutMs) override;
    bool wakeup() noexcept override;

    std::size_t registrationCount() const;

private:
    struct Registration {
        ReadinessRegistrationIdentity identity{};
        Channel* target{nullptr};
        std::uint32_t interests{0};
        bool inKernel{false};
    };

    void assertOwnerThread() const;
    std::uint64_t allocateGeneration(SocketFd source);
    void updateKernel(
        int operation,
        SocketFd source,
        std::uint64_t generation,
        std::uint32_t interests);
    bool appendNativeNotice(
        std::uint64_t generation,
        std::uint32_t nativeEvents);
    void resetNoticeBatch() noexcept;
    ReadinessWaitResult currentResult(
        gamenet::base::Timestamp observedAt) const noexcept;

    EventLoop* ownerLoop_;
    ReadinessPortOptions options_;
    std::vector<epoll_event> nativeEvents_;
    std::vector<ReadinessNotice> notices_;
    int epollfd_;
    platform::WakeupFdPair wakeupFds_;
    std::unordered_map<SocketFd, Registration> registrations_;
    std::uint32_t nextGeneration_{1};
    ReadinessWaitProgress progress_{};

    friend class EpollReadinessPortHarness;
};

}  // namespace detail
}  // namespace gamenet::net

#endif
