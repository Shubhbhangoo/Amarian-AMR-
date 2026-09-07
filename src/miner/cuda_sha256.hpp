#pragma once

#include <array>
#include <cstdint>

namespace amarian::gpu {

/// Searches [start, start + attempts) for a nonce satisfying the RPC target.
/// The header is the canonical 92-byte serialized header with nonce bytes at offset 84.
[[nodiscard]] bool Mine(const std::array<uint8_t, 92>& header,
                        const std::array<uint8_t, 32>& target,
                        uint64_t start, uint64_t attempts,
                        unsigned blocks, unsigned threads, uint64_t& found_nonce);

} // namespace amarian::gpu
