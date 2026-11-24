// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include <gamenet/core/net/EventLoop.h>
#ifdef GAMENET_TIMER_DIAGNOSTICS
#include "observer.h"
#endif
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string_view>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Count = std::uint64_t;
Count stamp(Clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}
Count cpu() {
    timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) std::abort();
    return ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string_view mode(argv[1]);
    if (mode != "phase" && mode != "cadence") return 2;
    cpu_set_t mask{};
    if (::sched_getaffinity(0, sizeof(mask), &mask) != 0) return 3;
    int first = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &mask)) { first = i; break; }
    if (first < 0) return 3;
    CPU_ZERO(&mask);
    CPU_SET(first, &mask);
    if (::sched_setaffinity(0, sizeof(mask), &mask) != 0) return 3;
    constexpr std::array delays{100us, 250us, 500us, 750us, 900us};
    const unsigned count = mode == "phase" ? 2000 : 3000;
    struct Row { Count armed, due, fired; unsigned delay; };
    std::vector<Row> rows;
    rows.reserve(count);
    gamenet::net::EventLoop loop;
#ifdef GAMENET_TIMER_DIAGNOSTICS
    timer_diagnostics::Observer observer(loop);
#endif
    auto deadline = Clock::now() + 1ms;
    std::function<void()> arm;
    arm = [&] {
        const auto armed = Clock::now();
        const unsigned delay = mode == "phase" ? delays[rows.size() % delays.size()].count() : 1000;
        if (mode == "phase") deadline = armed + std::chrono::microseconds(delay);
        if (!loop.runAt(deadline, [&, armed, delay] {
            const auto fired = Clock::now();
            if (!loop.isInLoopThread() || fired < deadline) std::abort();
            rows.push_back({stamp(armed), stamp(deadline), stamp(fired), delay});
            if (rows.size() == count) { loop.quit(); return; }
            if (mode == "cadence") deadline += 1ms * (1 + (fired - deadline) / 1ms);
            arm();
        }).valid()) std::abort();
    };
    const auto begin = stamp(Clock::now()), startCpu = cpu();
    arm();
    loop.loop();
    const auto elapsedCpu = cpu() - startCpu, end = stamp(Clock::now());
    if (loop.phase() != gamenet::net::EventLoopPhase::Shutdown ||
        loop.callbackExceptionCount() || loop.pendingFunctorCount() ||
        loop.pendingControlSourceCount() || loop.attachedLifecycleNodeCount() ||
        loop.pendingLifecycleNodeCount()) std::abort();
    const std::filesystem::path output(argv[2]);
    std::ofstream ticks(output / "timer.csv"), metrics(output / "metrics.csv");
    ticks.exceptions(std::ios::failbit | std::ios::badbit);
    metrics.exceptions(std::ios::failbit | std::ios::badbit);
    ticks << "armed_ns,deadline_ns,callback_ns,delay_us\n";
    for (const auto& r : rows)
        ticks << r.armed << ',' << r.due << ',' << r.fired << ',' << r.delay << '\n';
    metrics << "begin_ns," << begin << "\nend_ns," << end << "\nowner_cpu_ns," << elapsedCpu
            << "\nowner_cpu," << first << "\nowner_tid," << ::getpid() << "\nticks," << rows.size() << '\n';
#ifdef GAMENET_TIMER_DIAGNOSTICS
    observer.save(output);
#endif
}
