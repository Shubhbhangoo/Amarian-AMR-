#include <amarian/consensus/asert.hpp>

#include <amarian/consensus/target.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace amarian::consensus {
namespace {

/// A non-negative integer held as five 64-bit little-endian limbs: 320 bits.
///
/// Five limbs rather than four because the multiplication of a target (up to
/// 2^256 - 1) by the cubic factor (just under 2^17) can reach 2^273, and a value
/// that has not yet been clamped must be able to hold its own intermediates.
/// Everything is later clamped to the floor, which is far smaller, so the extra
/// limb is only ever an overflow detector in practice. The top limb is always
/// zero in a value that has been clamped.
struct Wide {
    std::array<uint64_t, 5> limb{};
};

constexpr size_t LIMB_COUNT = 5;
constexpr uint64_t LIMB_BITS = 64;
constexpr size_t VALUE_BITS = LIMB_COUNT * LIMB_BITS;

/// The number of fixed-point fraction bits in the exponent (aserti3-2d's radix).
constexpr unsigned RADIX_BITS = 16;
constexpr uint64_t RADIX = 1U << RADIX_BITS;

/// The saturation point of the scaled exponent, in units of 1/2^16.
///
/// A schedule deviation of 2^32 / 2^16 = 2^16 half-lives already pushes the
/// target far past either clamp: beyond it difficulty is either the floor or 1
/// either way. The bound exists so the fixed-point exponent always fits a signed
/// 64-bit integer; it does not constrain any reachable chain.
constexpr uint64_t EXPONENT_SATURATION = 1ULL << 32;

/// Clamps `value` into `[low, high]`, both of which may be negative.
[[nodiscard]] constexpr int64_t Clamp(int64_t value, int64_t low, int64_t high) noexcept {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

[[nodiscard]] bool IsZero(const Wide& value) noexcept {
    for (const uint64_t limb : value.limb) {
        if (limb != 0) {
            return false;
        }
    }
    return true;
}

/// A 256-bit big-endian target as the low four limbs of a `Wide`.
[[nodiscard]] Wide FromTarget(const Target& target) noexcept {
    Wide value;
    for (size_t limb = 0; limb < 4; ++limb) {
        uint64_t word = 0;
        for (size_t byte = 0; byte < 8; ++byte) {
            // Limb `limb` holds value bits [64*limb, 64*limb + 64): the last
            // eight bytes of the big-endian target, least significant byte first.
            const size_t source = target.size() - 1 - (limb * 8 + byte);
            word |= static_cast<uint64_t>(target[source]) << (byte * 8);
        }
        value.limb[limb] = word;
    }
    return value;
}

/// `(hi, lo) = a * b + add`, the portable form of the 128-bit product.
///
/// The compiler's `__int128` is deliberately not used: consensus code is built
/// with `-Wpedantic -Werror`, and a rule that depends on a GNU extension cannot
/// claim to be reproducible on every platform the project targets. The product is
/// decomposed into 32-bit halves; the largest intermediate of the standard
/// formulation stays well below 2^64.
///
/// Both carries are weighted, which is the whole difficulty of writing this by
/// hand. `add` enters at bit 0 and so is added to the *low* word, with its own
/// overflow carried up; and the carry out of `p01 + p10` is a bit at position 64
/// of a value that is itself shifted left by 32, so it belongs at bit 32 of the
/// high word rather than at bit 0. Getting either weight wrong produces a result
/// that is correct for small operands and wrong for large ones — which is exactly
/// the shape of bug that the shipped parameter table cannot see, because every
/// network's anchor target has its three mantissa bytes low in a limb and so no
/// limb product ever carries. `AnAnchorHighInAWordSurvivesTheFractionalMultiply`
/// in the ASERT test moves the anchor up a word so that one does.
void MulAdd64(uint64_t a, uint64_t b, uint64_t add, uint64_t& lo, uint64_t& hi) noexcept {
    const uint64_t a_lo = a & 0xFFFFFFFFU;
    const uint64_t a_hi = a >> 32;
    const uint64_t b_lo = b & 0xFFFFFFFFU;
    const uint64_t b_hi = b >> 32;

    const uint64_t p00 = a_lo * b_lo;
    const uint64_t p01 = a_lo * b_hi;
    const uint64_t p10 = a_hi * b_lo;
    const uint64_t p11 = a_hi * b_hi;

    // p01 + p10 can reach 2^65 - 2^33 + 1, so its overflow is captured rather
    // than let through.
    const uint64_t mid_lo = p01 + p10;
    const uint64_t mid_carry = mid_lo < p01 ? 1U : 0U;

    const uint64_t low = (mid_lo << 32) + p00;
    const uint64_t low_carry = low < p00 ? 1U : 0U;

    const uint64_t with_add = low + add;
    const uint64_t add_carry = with_add < low ? 1U : 0U;

    lo = with_add;
    // p11 <= (2^32 - 1)^2 = 2^64 - 2^33 + 1, and the three remaining terms are at
    // most 2^32 + (2^32 - 1) + 1 + 1, so the sum cannot reach 2^64.
    hi = p11 + (mid_carry << 32) + (mid_lo >> 32) + low_carry + add_carry;
}

/// `value >>= bits`, for any `bits`; everything is discarded past 320 bits.
void ShiftRight(Wide& value, uint64_t bits) noexcept {
    if (bits >= VALUE_BITS) {
        value = Wide{};
        return;
    }
    const size_t limb_shift = bits / LIMB_BITS;
    const unsigned bit_shift = static_cast<unsigned>(bits % LIMB_BITS);
    Wide shifted;
    for (size_t i = 0; i < LIMB_COUNT; ++i) {
        if (i + limb_shift >= LIMB_COUNT) {
            break;
        }
        uint64_t word = value.limb[i + limb_shift] >> bit_shift;
        if (bit_shift != 0 && i + limb_shift + 1 < LIMB_COUNT) {
            word |= value.limb[i + limb_shift + 1] << (LIMB_BITS - bit_shift);
        }
        shifted.limb[i] = word;
    }
    value = shifted;
}

/// `value <<= bits`, returning whether any set bit was pushed past 320 bits.
bool ShiftLeft(Wide& value, uint64_t bits) noexcept {
    if (bits == 0) {
        return false;
    }
    if (bits >= VALUE_BITS) {
        const bool nonzero = !IsZero(value);
        value = Wide{};
        return nonzero;
    }
    const size_t limb_shift = bits / LIMB_BITS;
    const unsigned bit_shift = static_cast<unsigned>(bits % LIMB_BITS);
    Wide shifted;
    for (size_t destination = 0; destination < LIMB_COUNT; ++destination) {
        uint64_t word = 0;
        if (destination >= limb_shift) {
            const size_t source = destination - limb_shift;
            word = value.limb[source] << bit_shift;
            if (bit_shift != 0 && source >= 1) {
                word |= value.limb[source - 1] >> (LIMB_BITS - bit_shift);
            }
        }
        shifted.limb[destination] = word;
    }

    // Overflow is about the *source* bits that do not fit, not about the wrapped
    // top limb. A source limb whose low part lands at or past limb 4, or whose
    // high part lands past limb 4, is discarded - and the wrapped destination
    // limb cannot see it, so it must be detected here.
    bool overflow = false;
    for (size_t source = 0; source < LIMB_COUNT; ++source) {
        if (source + limb_shift >= LIMB_COUNT) {
            if (value.limb[source] != 0) {
                overflow = true;
            }
        } else if (bit_shift != 0 && source + limb_shift == LIMB_COUNT - 1) {
            if ((value.limb[source] >> (LIMB_BITS - bit_shift)) != 0) {
                overflow = true;
            }
        }
    }
    value = shifted;
    return overflow;
}

/// `value *= factor`, returning whether any set bit was pushed past 320 bits.
bool MultiplySmall(Wide& value, uint64_t factor) noexcept {
    uint64_t carry = 0;
    for (size_t i = 0; i < LIMB_COUNT; ++i) {
        uint64_t lo = 0;
        uint64_t hi = 0;
        MulAdd64(value.limb[i], factor, carry, lo, hi);
        value.limb[i] = lo;
        carry = hi;
    }
    return carry != 0;
}

/// Whether `value` (320-bit) is greater than the 256-bit `limit` (low four limbs).
[[nodiscard]] bool Exceeds(const Wide& value, const Wide& limit) noexcept {
    for (size_t i = LIMB_COUNT; i-- > 0;) {
        if (value.limb[i] != limit.limb[i]) {
            return value.limb[i] > limit.limb[i];
        }
    }
    return false;
}

/// The low 256 bits of `value` as a big-endian target.
[[nodiscard]] Target ToTarget(const Wide& value) noexcept {
    Target target{};
    for (size_t limb = 0; limb < 4; ++limb) {
        const uint64_t word = value.limb[limb];
        for (size_t byte = 0; byte < 8; ++byte) {
            const size_t destination = target.size() - 1 - (limb * 8 + byte);
            target[destination] = static_cast<uint8_t>((word >> (byte * 8)) & 0xFFU);
        }
    }
    return target;
}

/// `floor(high * 2^16 / divisor)`, saturated at `EXPONENT_SATURATION`.
///
/// Long division over the 80-bit numerator `high << 16`, one bit at a time, so no
/// intermediate needs more than 64 bits: the running remainder is always below
/// `divisor` (clamped below 2^62) and the quotient exits as soon as it reaches
/// the saturation point, which is monotone. This is the exact truncating division
/// the published algorithm specifies, not an approximation.
[[nodiscard]] uint64_t ScaledQuotient(uint64_t high, uint64_t divisor) noexcept {
    if (divisor == 0 || divisor > (1ULL << 62)) {
        // A zero half-life is a caller error; the saturating answer is the only
        // deterministic one. A divisor above the clamp is treated the same way as
        // a huge half-life by never being smaller in practice.
        return EXPONENT_SATURATION;
    }
    uint64_t remainder = 0;
    uint64_t quotient = 0;
    for (int bit = 79; bit >= 0; --bit) {
        const uint64_t numerator_bit =
            bit >= 16 ? ((high >> (bit - 16)) & 1U) : 0U;
        remainder = (remainder << 1) | numerator_bit;
        uint64_t quotient_bit = 0;
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient_bit = 1;
        }
        quotient = (quotient << 1) | quotient_bit;
        if (quotient >= EXPONENT_SATURATION) {
            return EXPONENT_SATURATION;
        }
    }
    return quotient;
}

/// The cubic approximation of `2^fraction` scaled by `RADIX`, the published
/// aserti3-2d polynomial. Exact at the ends (1 at 0, 2 at 1) so the
/// split-and-shift identity has no discontinuity at the domain edges; error
/// below 0.013% in between. The polynomial is evaluated in unsigned 64-bit
/// arithmetic: at the largest fraction it sums to 18 446 563 080 438 344 768,
/// which fits, while a signed 64-bit integer would overflow.
[[nodiscard]] constexpr uint64_t FractionFactor(uint64_t fraction) noexcept {
    const uint64_t linear = 195766423245049ULL * fraction;
    const uint64_t quadratic = 971821376ULL * fraction * fraction;
    const uint64_t cubic = 5127ULL * fraction * fraction * fraction;
    const uint64_t rounding = 1ULL << 47;
    const uint64_t polynomial = linear + quadratic + cubic + rounding;
    return RADIX + (polynomial >> 48);
}

}  // namespace

uint32_t AsertNextBits(uint32_t anchor_bits, int64_t anchor_time, int64_t height_delta,
                       int64_t tip_time, int64_t spacing_seconds, int64_t half_life_seconds,
                       uint32_t pow_limit_bits) noexcept {
    const std::optional<Target> floor = CompactToTarget(pow_limit_bits);
    const std::optional<Target> anchor = CompactToTarget(anchor_bits);
    // The parameter table and `CheckBlockHeader` make these unreachable for every
    // network: `pow_limit_bits` and `genesis_bits` are canonical by construction,
    // and regtest — the only network whose floor is near 2^255 — never retargets.
    if (!floor.has_value() || !anchor.has_value()) {
        return pow_limit_bits;
    }

    Wide limit = FromTarget(*floor);
    Wide target = FromTarget(*anchor);
    if (Exceeds(target, limit)) {
        target = limit;
    }

    // Height cannot be negative, and a caller asking for a target "before" the
    // anchor has misused the function; the schedule deviation it implies is
    // nonsense, so the answer is simply the anchor's own target.
    if (height_delta <= 0) {
        return TargetToCompact(ToTarget(target));
    }

    // The inputs are clamped into ranges where every intermediate below is
    // provably representable. The clamps sit far beyond anything a real chain can
    // reach (2^61 seconds is tens of billions of years; 2^31 blocks and a 2^31
    // second spacing are each far past the protocol's own bounds), so they are a
    // totality guarantee for the rule, not a behaviour a network can observe.
    const int64_t bound = 1LL << 61;
    const int64_t origin = Clamp(anchor_time, -bound, bound);
    const int64_t observed = Clamp(tip_time, -bound, bound);
    const int64_t spacing = Clamp(spacing_seconds, 1, 1LL << 31);
    const int64_t height = Clamp(height_delta, 1, 1LL << 31);

    const int64_t time_delta = observed - origin;
    // Both operands are non-negative and bounded, so the product is at most 2^62.
    const int64_t expected = spacing * height;
    const int64_t numerator = time_delta - expected;

    // The signed fixed-point exponent, truncating toward zero as the published
    // algorithm requires.
    const uint64_t magnitude =
        numerator < 0 ? 0U - static_cast<uint64_t>(numerator)
                      : static_cast<uint64_t>(numerator);
    const uint64_t half_life = static_cast<uint64_t>(Clamp(half_life_seconds, 1, 1LL << 62));
    const uint64_t scaled = ScaledQuotient(magnitude, half_life);
    const int64_t exponent = numerator < 0 ? -static_cast<int64_t>(scaled)
                                           : static_cast<int64_t>(scaled);

    // Decompose into an integer shift and a 16-bit fraction. The arithmetic right
    // shift floors, so `fraction` below is the non-negative remainder and the two
    // parts recombine exactly.
    const int64_t integer_shift = exponent >> RADIX_BITS;
    const uint64_t fraction = static_cast<uint64_t>(exponent) & (RADIX - 1);

    // target = anchor * RADIX * 2^fraction, held in 320-bit intermediates.
    const uint64_t factor = FractionFactor(fraction);
    const bool multiplied_overflow = MultiplySmall(target, factor);

    // One combined shift performs the RADIX division and the integer part.
    const int64_t shift = integer_shift - static_cast<int64_t>(RADIX_BITS);
    if (shift > 0) {
        // Set bits pushed past 320 bits mean the value is at least 2^256, which
        // exceeds the floor; the floor is the answer.
        if (ShiftLeft(target, static_cast<uint64_t>(shift)) || multiplied_overflow) {
            return TargetToCompact(ToTarget(limit));
        }
    } else if (shift < 0) {
        ShiftRight(target, static_cast<uint64_t>(-shift));
    }

    if (IsZero(target)) {
        // Zero is not an encodable target; the hardest valid one is 1.
        Target hardest{};
        hardest[hardest.size() - 1] = 1;
        return TargetToCompact(hardest);
    }
    if (Exceeds(target, limit)) {
        return TargetToCompact(ToTarget(limit));
    }
    return TargetToCompact(ToTarget(target));
}

uint32_t AsertNextBits(const ChainParams& params, uint32_t tip_height, int64_t tip_time) noexcept {
    return AsertNextBits(params.genesis_bits, params.genesis_timestamp, tip_height, tip_time,
                         params.target_block_seconds, params.asert_half_life_seconds,
                         params.pow_limit_bits);
}

}  // namespace amarian::consensus
