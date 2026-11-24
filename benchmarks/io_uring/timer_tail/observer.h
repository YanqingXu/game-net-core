// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <gamenet/core/net/EventLoop.h>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace timer_diagnostics {
// Diagnostic consumer only; not an installed SDK API.
class Observer {
public:
    struct Row {
        std::uint64_t wall, cpu;
        int kind;
        std::int64_t value, extra;
    };
    explicit Observer(gamenet::net::EventLoop& loop);
    ~Observer();
    void save(const std::filesystem::path& directory) const;
    void mark(int kind, std::int64_t value = 0, std::int64_t extra = 0);
    static Observer* current() noexcept;
private:
    gamenet::net::EventLoop& loop_;
    std::vector<Row> rows_;
};

class CallbackSpan {
public:
    CallbackSpan() { if (auto* p = Observer::current()) p->mark(2); }
    ~CallbackSpan() { if (auto* p = Observer::current()) p->mark(3); }
};
}  // namespace timer_diagnostics
