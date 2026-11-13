// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
// Derived from intents/modules/kcp_transport.intent.md: fixed storage, atomic input.
#include "KcpEngine.h"
// clang-format off
#include "KcpSymbols.h"
#include "../../third_party/kcp/ikcp.h"
// clang-format on
#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#undef iqueue_entry
#define iqueue_entry(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))

namespace gamenet::kcp {
void KcpOptions::validate() const {
    udp.validate();
    if (!maxSessions || maxSessions > udp.maxPeers || maxSessions > 4096 ||
        maxSessionOpenings < maxSessions || maxSessionOpenings > 65536 || mtu < 512 || mtu > 1200 ||
        mtu > udp.socket.maxDatagramBytes || !sendWindow || sendWindow > maxSendSegments ||
        receiveWindow < 128 || receiveWindow > 512 || !maxSendSegments || maxSendSegments > 4096 ||
        !maxAckEntries || maxAckEntries > 4096 || !maxMessageBytes ||
        maxMessageBytes > (mtu - 24) * std::min<std::size_t>(127, maxSendSegments) ||
        !maxPendingMessagesPerSession || maxPendingMessagesPerSession > maxPendingMessages ||
        maxPendingMessages > 65536 || maxPendingBytesPerSession < maxMessageBytes ||
        maxPendingBytesPerSession > maxPendingBytes || maxPendingBytes > 64 * 1024 * 1024 ||
        !maxProtocolBytes || maxProtocolBytes > 1024ull * 1024 * 1024 || !maxSessionsPerTurn ||
        maxSessionsPerTurn > maxSessions || !maxMessagesPerSessionTurn ||
        maxMessagesPerSessionTurn > 1024 || interval < std::chrono::milliseconds(10) ||
        interval > std::chrono::milliseconds(100) || sendTimeout < interval ||
        sendTimeout > std::chrono::hours(1) || idleTimeout < interval ||
        idleTimeout > std::chrono::hours(24) || udp.idleTimeout < idleTimeout ||
        maxTransmissions < 2 || maxTransmissions > 100)
        throw std::invalid_argument("invalid KCP budgets/timers/windows");
}
namespace detail {
namespace {
struct Exhausted {};
constexpr std::size_t alignment = alignof(std::max_align_t);
constexpr std::size_t align(std::size_t n) { return (n + alignment - 1) / alignment * alignment; }
struct Layout {
    std::size_t small, count, large;
    explicit Layout(const KcpOptions& o)
        : small(align(std::max(sizeof(IKCPSEG) + o.mtu - 24, sizeof(IKCPCB)))),
          count(o.maxSendSegments + 2 * o.receiveWindow + 3),
          large(align(std::max({std::size_t(4272), (o.mtu + 24) * 3, o.maxAckEntries * 8}))) {}
    std::size_t bytes() const { return count * small + 3 * large; }
};
class Arena {
  public:
    explicit Arena(Layout l) : layout(l), slab(new std::byte[l.bytes()]), used(l.count + 3) {}
    void* allocate(std::size_t n) {
        if (exhaust) {
            exhaust = false;
            throw Exhausted{};
        }
        const bool small = n <= layout.small;
        const auto first = small ? 0 : layout.count, last = small ? layout.count : layout.count + 3;
        if (n > layout.large)
            throw Exhausted{};
        for (auto i = first; i < last; ++i)
            if (!used[i]) {
                used[i] = 1;
                ++occupied;
                return reinterpret_cast<char*>(slab.get()) + offset(i);
            }
        throw Exhausted{};
    }
    void release(void* p) noexcept {
        if (!p)
            return;
        const auto n = static_cast<char*>(p) - reinterpret_cast<char*>(slab.get());
        const auto split = layout.count * layout.small;
        const auto i = std::size_t(n) < split
                           ? std::size_t(n) / layout.small
                           : layout.count + (std::size_t(n) - split) / layout.large;
        if (n < 0 || i >= used.size() || offset(i) != std::size_t(n) || !used[i])
            std::terminate();
        used[i] = 0;
        --occupied;
    }
    std::size_t offset(std::size_t i) const {
        return i < layout.count ? i * layout.small
                                : layout.count * layout.small + (i - layout.count) * layout.large;
    }
    Layout layout;
    std::unique_ptr<std::byte[]> slab;
    std::vector<unsigned char> used;
    std::size_t occupied{};
    bool exhaust{};
};
thread_local Arena* currentArena{};
struct Scope {
    Arena* previous;
    explicit Scope(Arena& a) : previous(currentArena) { currentArena = &a; }
    ~Scope() { currentArena = previous; }
};
void* allocate(std::size_t n) {
    if (!currentArena)
        throw Exhausted{};
    return currentArena->allocate(n);
}
void release(void* p) {
    if (!currentArena)
        std::terminate();
    currentArena->release(p);
}
std::uint32_t read32(const char* p) {
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16) |
           (std::uint32_t(b[3]) << 24);
}
bool after(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) > 0; }
} // namespace
struct KcpEngine::Impl {
    const KcpOptions options;
    std::unique_ptr<Arena> arena;
    std::vector<char> scratch;
    ikcpcb* pcb{};
    Output output;
    void* user;
    bool error{};
    Impl(std::uint32_t conv, const KcpOptions& o, Output fn, void* ctx)
        : options(o), arena(std::make_unique<Arena>(Layout(o))), scratch(o.maxMessageBytes),
          output(fn), user(ctx) {
        static std::once_flag hooks;
        std::call_once(hooks, [] { ikcp_allocator(allocate, release); });
        Scope scope(*arena);
        pcb = ikcp_create(conv, this);
        if (!pcb || ikcp_setmtu(pcb, static_cast<int>(o.mtu)))
            throw std::bad_alloc();
        ikcp_wndsize(pcb, static_cast<int>(o.sendWindow), static_cast<int>(o.receiveWindow));
        ikcp_nodelay(pcb, 0, static_cast<int>(o.interval.count()), 2, 0);
        pcb->dead_link = o.maxTransmissions;
        pcb->acklist = static_cast<IUINT32*>(arena->allocate(o.maxAckEntries * 8));
        pcb->ackblock = static_cast<IUINT32>(o.maxAckEntries);
        ikcp_setoutput(pcb, [](const char* data, int len, ikcpcb*, void* ctx) {
            auto& self = *static_cast<Impl*>(ctx);
            return self.output(self.user, {data, static_cast<std::size_t>(len)});
        });
    }
    void fail() noexcept {
        error = true;
        pcb = nullptr;
        arena.reset();
    }
};
std::size_t KcpEngine::storageBytes(const KcpOptions& o) {
    Layout l(o);
    return l.bytes() + l.count + 3 + o.maxMessageBytes + sizeof(Impl) + sizeof(Arena);
}
KcpEngine::KcpEngine(std::uint32_t conv, const KcpOptions& o, Output output, void* user) {
    o.validate();
    if (!conv || !output)
        throw std::invalid_argument("invalid KCP engine");
    try {
        impl_ = std::make_unique<Impl>(conv, o, output, user);
    } catch (const Exhausted&) {
        throw std::bad_alloc();
    }
}
KcpEngine::~KcpEngine() = default; // Whole arena owns every allocation, no partial-list traversal.
std::optional<std::uint32_t> KcpEngine::send(std::string_view bytes) noexcept {
    auto& s = *impl_;
    if (s.error || bytes.size() > s.options.maxMessageBytes)
        return {};
    const auto count = std::max<std::size_t>(1, (bytes.size() + s.pcb->mss - 1) / s.pcb->mss);
    if (count + s.pcb->nsnd_buf + s.pcb->nsnd_que > s.options.maxSendSegments)
        return {};
    try {
        Scope scope(*s.arena);
        if (ikcp_send(s.pcb, bytes.data(), static_cast<int>(bytes.size())) !=
            static_cast<int>(bytes.size())) {
            s.fail();
            return {};
        }
        return s.pcb->snd_nxt + s.pcb->nsnd_que - 1;
    } catch (...) {
        s.fail();
        return {};
    }
}
InputResult KcpEngine::input(std::string_view bytes) noexcept {
    auto& s = *impl_;
    if (s.error)
        return InputResult::Failed;
    if (bytes.size() < 24 || bytes.size() > s.options.mtu)
        return InputResult::Invalid;
    std::size_t at = 0, pushes = 0;
    const auto maxFragments = (s.options.maxMessageBytes + s.pcb->mss - 1) / s.pcb->mss;
    while (at < bytes.size()) {
        if (bytes.size() - at < 24)
            return InputResult::Invalid;
        const auto* p = bytes.data() + at;
        const auto cmd = static_cast<unsigned char>(p[4]), frg = static_cast<unsigned char>(p[5]);
        const auto len = read32(p + 20);
        if (read32(p) != s.pcb->conv || cmd < 81 || cmd > 84 || len > bytes.size() - at - 24 ||
            len > s.pcb->mss || frg >= maxFragments || (cmd != 81 && (len || frg)) ||
            after(read32(p + 16), s.pcb->snd_nxt) ||
            // Bound upstream's signed RTT filter arithmetic. Standard peers
            // echo our timestamp; a sample beyond the maximum KCP RTO is stale.
            (cmd == 82 && static_cast<std::int32_t>(s.pcb->current - read32(p + 8)) > 60000))
            return InputResult::Invalid;
        if (cmd == 81)
            ++pushes;
        at += 24 + len;
    }
    if (pushes > s.options.maxAckEntries - s.pcb->ackcount)
        return InputResult::AckBudget;
    try {
        Scope scope(*s.arena);
        if (ikcp_input(s.pcb, bytes.data(), static_cast<long>(bytes.size())) != 0) {
            s.fail();
            return InputResult::Failed;
        }
        return InputResult::Accepted;
    } catch (...) {
        s.fail();
        return InputResult::Failed;
    }
}
bool KcpEngine::update(std::uint32_t now) noexcept {
    auto& s = *impl_;
    if (s.error)
        return false;
    try {
        Scope scope(*s.arena);
        ikcp_update(s.pcb, now);
        if (s.pcb->state == IUINT32(-1))
            s.fail();
    } catch (...) {
        s.fail();
    }
    return !s.error;
}
bool KcpEngine::acknowledged(std::uint32_t sn) const noexcept {
    return !impl_->error && after(impl_->pcb->snd_una, sn);
}
std::optional<std::string_view> KcpEngine::receive() noexcept {
    auto& s = *impl_;
    if (s.error)
        return {};
    const auto size = ikcp_peeksize(s.pcb);
    if (size < 0)
        return {};
    if (std::size_t(size) > s.scratch.size()) {
        s.fail();
        return {};
    }
    // Upstream peek/recv does not enforce a descending fragment chain.
    auto* node = s.pcb->rcv_queue.next;
    auto* seg = iqueue_entry(node, IKCPSEG, node);
    auto expected = seg->frg;
    for (;;) {
        if (node == &s.pcb->rcv_queue) {
            s.fail();
            return {};
        }
        seg = iqueue_entry(node, IKCPSEG, node);
        if (seg->frg != expected) {
            s.fail();
            return {};
        }
        if (!expected)
            break;
        --expected;
        node = node->next;
    }
    try {
        Scope scope(*s.arena);
        if (ikcp_recv(s.pcb, s.scratch.data(), static_cast<int>(s.scratch.size())) != size) {
            s.fail();
            return {};
        }
        return std::string_view(s.scratch.data(), static_cast<std::size_t>(size));
    } catch (...) {
        s.fail();
        return {};
    }
}
bool KcpEngine::failed() const noexcept { return impl_->error; }
EngineSnapshot KcpEngine::snapshot() const noexcept {
    const auto& s = *impl_;
    if (s.error)
        return {};
    std::size_t used = 0;
    for (std::size_t i = 0; i < s.arena->used.size(); ++i)
        if (s.arena->used[i])
            used += i < s.arena->layout.count ? s.arena->layout.small : s.arena->layout.large;
    return {s.pcb->nsnd_que + s.pcb->nsnd_buf, s.pcb->nrcv_que + s.pcb->nrcv_buf, s.pcb->ackcount,
            used, s.arena->layout.bytes()};
}
void KcpEngine::exhaustNextAllocationForTesting() noexcept {
    if (impl_->arena)
        impl_->arena->exhaust = true;
}
} // namespace detail
} // namespace gamenet::kcp
