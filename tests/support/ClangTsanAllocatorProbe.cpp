// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

// External detection control for the complete shared Clang TSan runtime.
// This is deliberately not a CTest target; "race" must report a heap data race.
#include "support/TestAssert.h"
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <new>
#include <string_view>
#include <thread>

void* operator new(std::size_t bytes) {
    if (auto* value = std::malloc(bytes ? bytes : 1))
        return value;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

int main(int argc, char** argv) {
    GAMENET_TEST_ASSERT(argc == 2);
    const std::string_view mode(argv[1]);
    GAMENET_TEST_ASSERT(mode == "atomic" || mode == "race");
    std::barrier start(2);
    if (mode == "race") {
        auto* value = new volatile int(0);
        std::thread first([&] { start.arrive_and_wait(); *value = 1; });
        std::thread second([&] { start.arrive_and_wait(); *value = 2; });
        first.join();
        second.join();
        delete value;
    } else {
        auto* value = new std::atomic<int>(0);
        std::thread first([&] { start.arrive_and_wait(); value->store(1); });
        std::thread second([&] { start.arrive_and_wait(); value->store(2); });
        first.join();
        second.join();
        GAMENET_TEST_ASSERT(value->load() == 1 || value->load() == 2);
        delete value;
    }
}
