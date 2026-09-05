#include <amarian/consensus/target.hpp>

namespace amarian {
namespace {

/// The bit the compact encoding reserves for a sign a target cannot have.
constexpr uint32_t MANTISSA_SIGN_BIT = 0x00800000U;
constexpr int TARGET_BYTES = 32;

}  // namespace

std::optional<Target> CompactToTarget(uint32_t bits) noexcept {
    const uint32_t exponent = bits >> 24;
    const uint32_t mantissa = bits & 0x00FFFFFFU;

    if ((mantissa & MANTISSA_SIGN_BIT) != 0) {
        return std::nullopt;
    }
    // The sign bit is already excluded, so this is simply a zero magnitude.
    if (mantissa == 0) {
        return std::nullopt;
    }

    // The value is mantissa * 256^(exponent - 3), so the mantissa's most
    // significant byte carries weight 256^(exponent - 1) and therefore lands at
    // index 32 - exponent of a big-endian 32-byte array. Signed arithmetic
    // throughout: an out-of-range index is the thing being detected, so it must be
    // representable rather than wrapping.
    Target target{};
    const int most_significant = TARGET_BYTES - static_cast<int>(exponent);
    for (int offset = 0; offset < 3; ++offset) {
        const auto byte = static_cast<uint8_t>((mantissa >> (8 * (2 - offset))) & 0xFFU);
        const int index = most_significant + offset;
        if (index < 0 || index >= TARGET_BYTES) {
            // Below the array: the byte overflows 256 bits. Above it: the byte
            // would be truncated away. A canonical encoding does neither, so a
            // nonzero byte in either place is invalid rather than something to
            // silently drop.
            if (byte != 0) {
                return std::nullopt;
            }
            continue;
        }
        target[static_cast<size_t>(index)] = byte;
    }

    // One encoding per target. Everything not already rejected above — a
    // needlessly large exponent, a mantissa that is not left-aligned — fails here.
    if (TargetToCompact(target) != bits) {
        return std::nullopt;
    }
    return target;
}

uint32_t TargetToCompact(const Target& target) noexcept {
    size_t first = 0;
    while (first < target.size() && target[first] == 0) {
        ++first;
    }
    if (first == target.size()) {
        return 0;
    }

    // Significant length in bytes, which is exactly the exponent: the value is
    // mantissa * 256^(size - 3).
    uint32_t size = static_cast<uint32_t>(target.size() - first);

    // The three most significant bytes, left-aligned. Reading past the end of the
    // array contributes zeros, which is the same left-alignment a value shorter
    // than three bytes needs.
    uint32_t mantissa = 0;
    for (size_t offset = 0; offset < 3; ++offset) {
        mantissa <<= 8;
        if (first + offset < target.size()) {
            mantissa |= target[first + offset];
        }
    }

    // A mantissa whose top byte would set the sign bit is shifted down a byte and
    // the exponent raised to compensate, so the sign bit is never set by a
    // magnitude.
    if ((mantissa & MANTISSA_SIGN_BIT) != 0) {
        mantissa >>= 8;
        ++size;
    }

    return (size << 24) | mantissa;
}

bool HashMeetsTarget(const Hash256& hash, const Target& target) noexcept {
    const uint8_t* digest = hash.Data();
    for (size_t offset = 0; offset < target.size(); ++offset) {
        // Display order: the digest's last byte is the most significant.
        const uint8_t hash_byte = digest[Hash256::SIZE - 1 - offset];
        if (hash_byte != target[offset]) {
            return hash_byte < target[offset];
        }
    }
    // Exactly equal to the target is valid work: the rule is "at most".
    return true;
}

bool CheckProofOfWork(const Hash256& hash, uint32_t bits) noexcept {
    const std::optional<Target> target = CompactToTarget(bits);
    if (!target.has_value()) {
        return false;
    }
    return HashMeetsTarget(hash, *target);
}

}  // namespace amarian
