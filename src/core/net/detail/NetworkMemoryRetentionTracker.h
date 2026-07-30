#pragma once

#include <cstddef>

namespace gamenet::net::detail {

enum class NetworkFixedStorageCategory {
    AcceptExFixedPool,
    IocpCompletionBatch,
    ConnectionLocalRead,
};

void retainNetworkFixedStorage(
    NetworkFixedStorageCategory category,
    std::size_t bytes) noexcept;
void releaseNetworkFixedStorage(
    NetworkFixedStorageCategory category,
    std::size_t bytes) noexcept;

}  // namespace gamenet::net::detail
