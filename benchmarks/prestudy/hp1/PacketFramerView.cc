// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0

#include "hp1/PacketFramerView.h"

#include <cstddef>
#include <utility>

namespace gamenet::experimental::hp1 {

PacketFramerViewPrototype::PacketFramerViewPrototype(
    protocol::PacketFramerOptions options)
    : options_(std::move(options)) {
    // The unchanged installed implementation remains the validation authority
    // for coherent payload, buffer, processing, and retention limits.
    (void)protocol::PacketFramer(options_);
}

bool PacketFramerViewPrototype::reset() noexcept {
    if (visitActive_) return false;
    faulted_ = false;
    return true;
}

std::size_t PacketFramerViewPrototype::payloadLength(
    std::span<const std::byte> input,
    std::size_t offset) noexcept {
    return
        (static_cast<std::size_t>(std::to_integer<unsigned char>(input[offset])) << 24U) |
        (static_cast<std::size_t>(std::to_integer<unsigned char>(input[offset + 1])) << 16U) |
        (static_cast<std::size_t>(std::to_integer<unsigned char>(input[offset + 2])) << 8U) |
        static_cast<std::size_t>(std::to_integer<unsigned char>(input[offset + 3]));
}

}  // namespace gamenet::experimental::hp1
