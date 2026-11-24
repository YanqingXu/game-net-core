// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "observer.h"

#include <sys/epoll.h>
#include <signal.h>
#include <time.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>

namespace timer_diagnostics {
namespace {
thread_local Observer* active = nullptr;
constexpr std::size_t limit = 2'000'000;
std::uint64_t clockNs(clockid_t id) {
    timespec ts{};
    if (::clock_gettime(id, &ts) != 0) std::abort();
    return ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}
}

Observer::Observer(gamenet::net::EventLoop& loop) : loop_(loop) {
    if (active || !loop.isInLoopThread()) std::abort();
    rows_.reserve(limit);
    active = this;
    loop_.setEventLoopMetricCallback([this](const gamenet::net::EventLoopMetricSample& s) {
        mark(10 + static_cast<int>(s.event), s.drainedWork, s.remainingWork);
    });
}
Observer::~Observer() {
    loop_.setEventLoopMetricCallback({});
    active = nullptr;
}
Observer* Observer::current() noexcept { return active; }
void Observer::mark(int kind, std::int64_t value, std::int64_t extra) {
    if (rows_.size() == limit) std::abort();  // Never silently drop trace data.
    rows_.push_back({clockNs(CLOCK_MONOTONIC), clockNs(CLOCK_THREAD_CPUTIME_ID),
                     kind, value, extra});
}
void Observer::save(const std::filesystem::path& directory) const {
    std::ofstream out(directory / "observer.csv");
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << "wall_ns,cpu_ns,kind,value,extra\n";
    for (const auto& r : rows_)
        out << r.wall << ',' << r.cpu << ',' << r.kind << ',' << r.value << ',' << r.extra << '\n';
}
}

extern "C" int __real_epoll_wait(int, epoll_event*, int, int);
extern "C" int __real_epoll_pwait2(int, epoll_event*, int, const timespec*, const sigset_t*);

extern "C" int __wrap_epoll_wait(int fd, epoll_event* events, int max, int timeout) {
    auto* observer = timer_diagnostics::Observer::current();
    const int incomingError = errno;
    if (observer) observer->mark(0, timeout < 0 ? -1 : timeout * 1'000'000LL, 0);
    errno = incomingError;
    const int result = __real_epoll_wait(fd, events, max, timeout);
    const int error = errno;
    if (observer) observer->mark(1, result, result < 0 ? error : 0);
    errno = error;
    return result;
}
extern "C" int __wrap_epoll_pwait2(int fd, epoll_event* events, int max,
                                    const timespec* timeout, const sigset_t* mask) {
    auto* observer = timer_diagnostics::Observer::current();
    const int incomingError = errno;
    if (observer) observer->mark(0, timeout ? timeout->tv_sec * 1'000'000'000LL + timeout->tv_nsec : -1, 1);
    errno = incomingError;
    const int result = __real_epoll_pwait2(fd, events, max, timeout, mask);
    const int error = errno;
    if (observer) observer->mark(1, result, result < 0 ? error : 0);
    errno = error;
    return result;
}
