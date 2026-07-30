#pragma once

// Process-level accounting for Reactor/TCP transport-internal pool, slab, and
// fixed working storage not represented by logical pending-byte budgets.

#include <cstddef>

namespace gamenet::net {

struct NetworkFixedStorageRetentionSnapshot {
    // The active implementation intentionally has no shared read pool/slab.
    std::size_t sharedReadPoolBytes{};
    std::size_t sharedReadSlabBytes{};

    std::size_t acceptExFixedPoolBytes{};
    std::size_t peakAcceptExFixedPoolBytes{};
    std::size_t iocpCompletionBatchBytes{};
    std::size_t peakIocpCompletionBatchBytes{};
    std::size_t connectionLocalReadBytes{};
    std::size_t peakConnectionLocalReadBytes{};

    std::size_t totalRetainedBytes{};
    std::size_t peakTotalRetainedBytes{};

    // Platform inventory limits. Unsupported backend categories report zero.
    std::size_t acceptExSlotLimitPerAcceptor{};
    std::size_t iocpCompletionBatchEntriesPerLoop{};
    std::size_t connectionLocalReadChunkLimitBytes{};
};

// Cross-thread-safe, low-frequency process snapshot. Individual atomic fields
// can observe slightly different instants while another thread allocates or
// releases a category; every current counter remains exact and every peak is
// monotonic.
NetworkFixedStorageRetentionSnapshot
networkFixedStorageRetentionSnapshot() noexcept;

}  // namespace gamenet::net
