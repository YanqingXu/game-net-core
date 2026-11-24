// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Diagnostic executable only. No runtime API, allocator or lifetime changes.
#include "observer.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <sys/resource.h>

namespace {
struct Row { std::uintptr_t pc{}; unsigned kind{}; std::uint64_t calls{}, bytes{}; };
thread_local bool enabled = false;
thread_local std::array<Row, 8192> rows{};
thread_local bool overflow = false;
thread_local rusage first{}, last{};
void count(unsigned kind, void* caller, std::size_t bytes) noexcept {
    if (!enabled) return;
    const auto pc = reinterpret_cast<std::uintptr_t>(caller);
    auto index = ((pc >> 3) ^ kind) % rows.size();
    for (std::size_t n = 0; n < rows.size(); ++n, index = (index + 1) % rows.size()) {
        auto& row = rows[index];
        if (!row.pc || (row.pc == pc && row.kind == kind)) {
            row.pc = pc; row.kind = kind; ++row.calls; row.bytes += bytes; return;
        }
    }
    overflow = true;
}
void* allocate(std::size_t n, std::size_t alignment, void* pc) {
    void* p = nullptr;
    if (alignment) { if (::posix_memalign(&p, alignment, n ? n : 1)) p = nullptr; }
    else p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    count(0, pc, n);
    return p;
}
}
#define CALLER __builtin_extract_return_addr(__builtin_return_address(0))
void* operator new(std::size_t n) { return allocate(n, 0, CALLER); }
void* operator new[](std::size_t n) { return allocate(n, 0, CALLER); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate(n, std::size_t(a), CALLER); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate(n, std::size_t(a), CALLER); }
void operator delete(void* p) noexcept { if(p) count(1, CALLER, 0); std::free(p); }
void operator delete[](void* p) noexcept { if(p) count(1, CALLER, 0); std::free(p); }
void operator delete(void* p, std::size_t) noexcept { if(p) count(1, CALLER, 0); std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { if(p) count(1, CALLER, 0); std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { if(p) count(1, CALLER, 0); std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { if(p) count(1, CALLER, 0); std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { if(p) count(1, CALLER, 0); std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { if(p) count(1, CALLER, 0); std::free(p); }
extern "C" {
void* __real_memcpy(void*, const void*, std::size_t);
void* __real_memmove(void*, const void*, std::size_t);
void* __real_memset(void*, int, std::size_t);
void* __wrap_memcpy(void* d, const void* s, std::size_t n) { count(2, CALLER, n); return __real_memcpy(d,s,n); }
void* __wrap_memmove(void* d, const void* s, std::size_t n) { count(3, CALLER, n); return __real_memmove(d,s,n); }
void* __wrap_memset(void* d, int c, std::size_t n) { count(4, CALLER, n); return __real_memset(d,c,n); }
}
namespace cost_ledger {
void begin() {
    if (enabled || getrusage(RUSAGE_THREAD, &first)) std::abort();
    enabled = true;
}
void end() {
    enabled = false;
    if (getrusage(RUSAGE_THREAD, &last)) std::abort();
}
void save(const char* directory) {
    if (!directory || enabled || overflow) throw std::runtime_error("cost observer boundary/overflow");
    const std::filesystem::path root(directory);
    std::ofstream out(root / "cost.csv");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "kind,pc,calls,bytes\n";
    for (const auto& row : rows) if(row.pc)
        out << row.kind << ',' << std::hex << row.pc - 1 << std::dec << ',' << row.calls << ',' << row.bytes << '\n';
    auto ns = [](timeval v) { return std::uint64_t(v.tv_sec)*1'000'000'000 + v.tv_usec*1000; };
    std::ofstream usage(root / "usage.csv");
    usage.exceptions(std::ios::badbit | std::ios::failbit);
    usage << "user_ns," << ns(last.ru_utime) - ns(first.ru_utime) << '\n'
          << "system_ns," << ns(last.ru_stime) - ns(first.ru_stime) << '\n'
          << "voluntary_switches," << last.ru_nvcsw-first.ru_nvcsw << '\n'
          << "involuntary_switches," << last.ru_nivcsw-first.ru_nivcsw << '\n';
}
}
