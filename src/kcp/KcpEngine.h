// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gamenet/kcp/KcpTransport.h"
#include <optional>
#include <span>
#include <vector>
namespace gamenet::kcp::detail {
enum class InputResult { Accepted, Invalid, AckBudget, Failed };
struct EngineSnapshot {
    std::size_t sendSegments{}, receiveSegments{}, ackEntries{}, arenaUsed{}, arenaCapacity{};
};
class KcpEngine final {
  public:
    using Output = int (*)(void*, std::string_view) noexcept;
    KcpEngine(std::uint32_t conv, const KcpOptions&, Output, void*);
    ~KcpEngine();
    KcpEngine(const KcpEngine&) = delete;
    KcpEngine& operator=(const KcpEngine&) = delete;
    static std::size_t storageBytes(const KcpOptions&);
    // Nullopt rejects without mutation. endSequence is the last fragment's sequence.
    std::optional<std::uint32_t> send(std::string_view) noexcept;
    InputResult input(std::string_view) noexcept;
    bool update(std::uint32_t milliseconds) noexcept;
    bool acknowledged(std::uint32_t endSequence) const noexcept;
    // Borrowed buffer valid until next receive. No application callback inside this class.
    std::optional<std::string_view> receive() noexcept;
    bool failed() const noexcept;
    EngineSnapshot snapshot() const noexcept;
    // Source-private fault injection: exhaust the next native allocation.
    void exhaustNextAllocationForTesting() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gamenet::kcp::detail
