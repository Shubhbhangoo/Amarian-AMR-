#include <amarian/util/hex.hpp>
#include <amarian/util/types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace amarian {

Hash256 Hash256::FromBytes(ByteSpan bytes) {
    if (bytes.size() != SIZE) {
        throw std::invalid_argument("Hash256::FromBytes: expected exactly 32 bytes");
    }
    std::array<uint8_t, SIZE> arr{};
    std::ranges::copy(bytes, arr.begin());
    return Hash256(arr);
}

std::string Hash256::ToHexInternal() const {
    return amarian::ToHex(Span());
}

std::string Hash256::ToHex() const {
    std::array<uint8_t, SIZE> reversed{};
    std::ranges::reverse_copy(data_, reversed.begin());
    return amarian::ToHex(ByteSpan{reversed});
}

bool ConstantTimeEqual(ByteSpan a, ByteSpan b) noexcept {
    // Lengths are not secret in any Amarian use of this function, so an early return
    // on mismatch is fine. The byte loop below uses a volatile accumulator so the
    // compiler cannot introduce an early exit on the first differing byte.
    if (a.size() != b.size()) {
        return false;
    }
    volatile uint8_t acc = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        acc = static_cast<uint8_t>(acc | (a[i] ^ b[i]));
    }
    return acc == 0;
}

}  // namespace amarian
