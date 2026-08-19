#include "EpollReadinessPort.h"

#ifndef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/EventLoop.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace gamenet::net::detail {

namespace {

constexpr std::uint64_t kWakeupGeneration = 0;
constexpr std::size_t kMaxReadinessBatchSize = 4096;

[[noreturn]] void epollDie(const char* what) {
    LOG_SYSFATAL << what << ": " << std::strerror(errno);
    __builtin_unreachable();
}

std::uint32_t toNativeEvents(std::uint32_t interests) noexcept {
    std::uint32_t events = 0;
    if ((interests & Channel::kReadEvent) != 0) {
        events |= EPOLLIN | EPOLLPRI | EPOLLRDHUP;
    }
    if ((interests & Channel::kWriteEvent) != 0) {
        events |= EPOLLOUT;
    }
    return events;
}

std::uint32_t fromNativeEvents(std::uint32_t events) noexcept {
    std::uint32_t readiness = 0;
    if ((events & (EPOLLIN | EPOLLPRI)) != 0) {
        readiness |= Channel::kReadEvent;
    }
    if ((events & EPOLLOUT) != 0) {
        readiness |= Channel::kWriteEvent;
    }
    if ((events & EPOLLERR) != 0) {
        readiness |= Channel::kErrorEvent;
    }
    if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0) {
        readiness |= Channel::kCloseEvent;
    }
    return readiness;
}

bool validInterests(std::uint32_t interests) noexcept {
    constexpr std::uint32_t kAllowed =
        Channel::kReadEvent | Channel::kWriteEvent;
    return (interests & ~kAllowed) == 0;
}

ReadinessPortOptions validatedOptions(
    EventLoop* ownerLoop,
    ReadinessPortOptions options) {
    if (ownerLoop == nullptr) {
        throw std::invalid_argument(
            "EpollReadinessPort requires an owning EventLoop");
    }
    if (options.maxNoticesPerWait == 0 ||
        options.maxNoticesPerWait > kMaxReadinessBatchSize) {
        throw std::invalid_argument(
            "epoll readiness capacity must be within [1, 4096]");
    }
    if (options.triggerMode != ReadinessTriggerMode::Level) {
        throw std::invalid_argument(
            "EpollReadinessPort currently supports level triggering only");
    }
    return options;
}

}  // namespace

EpollReadinessPort::EpollReadinessPort(
    EventLoop* ownerLoop,
    ReadinessPortOptions options)
    : ownerLoop_(ownerLoop),
      options_(validatedOptions(ownerLoop, options)),
      nativeEvents_(options_.maxNoticesPerWait),
      notices_(options_.maxNoticesPerWait),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      wakeupFds_(platform::createWakeupFds()) {
    if (epollfd_ < 0) {
        epollDie("epoll_create1");
    }
    notices_.clear();

    epoll_event wakeupEvent{};
    wakeupEvent.events = EPOLLIN;
    wakeupEvent.data.u64 = kWakeupGeneration;
    if (::epoll_ctl(
            epollfd_,
            EPOLL_CTL_ADD,
            wakeupFds_.readFd,
            &wakeupEvent) < 0) {
        epollDie("epoll_ctl wakeup add");
    }
}

EpollReadinessPort::~EpollReadinessPort() {
    ::close(epollfd_);
    platform::closeWakeupFds(wakeupFds_);
}

ReadinessPortOptions EpollReadinessPort::options() const noexcept {
    return options_;
}

ReadinessRegistrationResult EpollReadinessPort::registerOrUpdate(
    ReadinessRegistrationRequest request) {
    assertOwnerThread();
    if (request.source == kInvalidSocket || request.target == nullptr ||
        request.target->ownerLoop() != ownerLoop_ ||
        !validInterests(request.interests)) {
        return {.result = ReadinessPortResult::RejectedInvalid};
    }

    auto existing = registrations_.find(request.source);
    if (existing == registrations_.end()) {
        if (request.interests == 0) {
            return {.result = ReadinessPortResult::RejectedInvalid};
        }

        const ReadinessRegistrationIdentity identity{
            .source = request.source,
            .generation = allocateGeneration(request.source),
        };
        const auto [registration, inserted] = registrations_.emplace(
            request.source,
            Registration{
                .identity = identity,
                .target = request.target,
                .interests = request.interests,
                .inKernel = false,
            });
        if (!inserted) {
            throw std::logic_error(
                "epoll readiness registration map insertion conflicted");
        }
        updateKernel(
            EPOLL_CTL_ADD,
            request.source,
            identity.generation,
            request.interests);
        registration->second.inKernel = true;
        return {
            .result = ReadinessPortResult::Accepted,
            .identity = identity,
        };
    }

    Registration& registration = existing->second;
    if (registration.target != request.target) {
        return {
            .result = ReadinessPortResult::RejectedConflict,
            .identity = registration.identity,
        };
    }
    if (registration.interests == request.interests) {
        return {
            .result = ReadinessPortResult::Accepted,
            .identity = registration.identity,
        };
    }

    if (registration.inKernel && request.interests == 0) {
        updateKernel(
            EPOLL_CTL_DEL,
            request.source,
            registration.identity.generation,
            0);
        registration.inKernel = false;
    } else if (!registration.inKernel && request.interests != 0) {
        updateKernel(
            EPOLL_CTL_ADD,
            request.source,
            registration.identity.generation,
            request.interests);
        registration.inKernel = true;
    } else if (registration.inKernel) {
        updateKernel(
            EPOLL_CTL_MOD,
            request.source,
            registration.identity.generation,
            request.interests);
    }
    registration.interests = request.interests;
    return {
        .result = ReadinessPortResult::Accepted,
        .identity = registration.identity,
    };
}

ReadinessPortResult EpollReadinessPort::cancel(Channel* target) {
    assertOwnerThread();
    if (target == nullptr || target->ownerLoop() != ownerLoop_) {
        return ReadinessPortResult::RejectedInvalid;
    }
    const auto existing = registrations_.find(target->fd());
    if (existing == registrations_.end()) {
        return ReadinessPortResult::RejectedNotRegistered;
    }
    if (existing->second.target != target) {
        return ReadinessPortResult::RejectedConflict;
    }

    const Registration registration = existing->second;
    if (registration.inKernel) {
        updateKernel(
            EPOLL_CTL_DEL,
            registration.identity.source,
            registration.identity.generation,
            0);
    }
    registrations_.erase(existing);
    return ReadinessPortResult::Accepted;
}

bool EpollReadinessPort::has(const Channel* target) const {
    assertOwnerThread();
    if (target == nullptr) {
        return false;
    }
    const auto existing = registrations_.find(target->fd());
    return existing != registrations_.end() &&
        existing->second.target == target;
}

std::optional<ReadinessRegistrationIdentity>
EpollReadinessPort::registrationIdentity(const Channel* target) const {
    assertOwnerThread();
    if (target == nullptr) {
        return std::nullopt;
    }
    const auto existing = registrations_.find(target->fd());
    if (existing == registrations_.end() ||
        existing->second.target != target) {
        return std::nullopt;
    }
    return existing->second.identity;
}

bool EpollReadinessPort::isCurrent(
    ReadinessRegistrationIdentity identity,
    const Channel* target) const {
    assertOwnerThread();
    if (!identity.valid() || target == nullptr) {
        return false;
    }
    const auto existing = registrations_.find(identity.source);
    return existing != registrations_.end() &&
        existing->second.target == target &&
        existing->second.identity == identity;
}

ReadinessWaitResult EpollReadinessPort::wait(int timeoutMs) {
    assertOwnerThread();
    resetNoticeBatch();
    const int count = ::epoll_wait(
        epollfd_,
        nativeEvents_.data(),
        static_cast<int>(nativeEvents_.size()),
        timeoutMs);
    const auto observedAt = gamenet::base::now();
    if (count < 0) {
        if (errno == EINTR) {
            return currentResult(observedAt);
        }
        epollDie("epoll_wait");
    }

    progress_.nativeNotices = static_cast<std::size_t>(count);
    progress_.budgetExhausted = count > 0 &&
        static_cast<std::size_t>(count) == nativeEvents_.size();
    for (int index = 0; index < count; ++index) {
        const epoll_event& event = nativeEvents_[index];
        if (event.data.u64 == kWakeupGeneration) {
            if (platform::drainWakeup(wakeupFds_.readFd)) {
                ++progress_.wakeupNotices;
            }
            continue;
        }
        (void)appendNativeNotice(event.data.u64, event.events);
    }
    progress_.deliveredNotices = notices_.size();
    return currentResult(observedAt);
}

bool EpollReadinessPort::wakeup() noexcept {
    const ssize_t written = platform::writeWakeup(wakeupFds_.writeFd);
    if (written >= 0) {
        return true;
    }
    return sockets::isWouldBlock(sockets::lastError());
}

std::size_t EpollReadinessPort::registrationCount() const {
    assertOwnerThread();
    return registrations_.size();
}

void EpollReadinessPort::assertOwnerThread() const {
    ownerLoop_->assertInLoopThread();
}

std::uint64_t EpollReadinessPort::allocateGeneration(SocketFd source) {
    if (nextGeneration_ == 0 ||
        nextGeneration_ == std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "epoll readiness registration generation exhausted");
    }
    // Linux descriptors are non-negative 32-bit ints. Packing the source in
    // the low word lets wait validate a token with the single registration
    // map; the high-word epoch prevents descriptor reuse from reviving an old
    // notice. Exhaustion fails closed before an epoch can repeat.
    const std::uint64_t epoch = nextGeneration_++;
    return (epoch << 32U) |
        static_cast<std::uint32_t>(source);
}

void EpollReadinessPort::updateKernel(
    int operation,
    SocketFd source,
    std::uint64_t generation,
    std::uint32_t interests) {
    epoll_event event{};
    event.events = toNativeEvents(interests);
    event.data.u64 = generation;
    epoll_event* eventPointer = operation == EPOLL_CTL_DEL ? nullptr : &event;
    if (::epoll_ctl(epollfd_, operation, source, eventPointer) < 0) {
        epollDie("epoll_ctl readiness registration");
    }
}

bool EpollReadinessPort::appendNativeNotice(
    std::uint64_t generation,
    std::uint32_t nativeEvents) {
    const SocketFd source = static_cast<SocketFd>(
        static_cast<std::uint32_t>(generation));
    const auto registration = registrations_.find(source);
    if (registration == registrations_.end() ||
        registration->second.identity.generation != generation ||
        !registration->second.inKernel) {
        ++progress_.staleNotices;
        return false;
    }

    const std::uint32_t alwaysDelivered =
        Channel::kErrorEvent | Channel::kCloseEvent;
    const std::uint32_t events = fromNativeEvents(nativeEvents) &
        (registration->second.interests | alwaysDelivered);
    if (events == 0) {
        return false;
    }
    for (auto& notice : notices_) {
        if (notice.identity == registration->second.identity) {
            notice.events |= events;
            return true;
        }
    }
    if (notices_.size() >= options_.maxNoticesPerWait) {
        progress_.budgetExhausted = true;
        return false;
    }
    notices_.push_back(ReadinessNotice{
        .identity = registration->second.identity,
        .target = registration->second.target,
        .events = events,
    });
    return true;
}

void EpollReadinessPort::resetNoticeBatch() noexcept {
    notices_.clear();
    progress_ = {};
}

ReadinessWaitResult EpollReadinessPort::currentResult(
    gamenet::base::Timestamp observedAt) const noexcept {
    return {
        .observedAt = observedAt,
        .notices = notices_,
        .progress = progress_,
    };
}

}  // namespace gamenet::net::detail

#endif
