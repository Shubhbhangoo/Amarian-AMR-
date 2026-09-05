#include <amarian/consensus/work.hpp>

#include <amarian/util/hex.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace amarian {
namespace {

using Bytes = std::array<uint8_t, Work::SIZE>;

constexpr size_t BIT_COUNT = Work::SIZE * 8;

/// Sets bit `index`, counting 0 as the least significant. Big-endian storage puts that
/// bit in the last byte, which is why the position is measured from the end.
void SetBit(Bytes& value, size_t index) noexcept {
    const size_t position = Work::SIZE - 1 - (index / 8);
    value[position] = static_cast<uint8_t>(value[position] | (1U << (index % 8)));
}

/// One past the highest set bit, or 0 for zero.
[[nodiscard]] size_t BitLength(const Bytes& value) noexcept {
    for (size_t position = 0; position < Work::SIZE; ++position) {
        if (value[position] != 0) {
            const size_t below = (Work::SIZE - 1 - position) * 8;
            return below + static_cast<size_t>(std::bit_width(value[position]));
        }
    }
    return 0;
}

/// `value <<= shift`, discarding anything shifted above 256 bits. Every caller here
/// shifts by an amount that provably keeps the value in range, so nothing is discarded
/// in practice; the truncation is what makes that safe to be wrong about.
void ShiftLeft(Bytes& value, size_t shift) noexcept {
    if (shift == 0) {
        return;
    }
    if (shift >= BIT_COUNT) {
        value.fill(0);
        return;
    }

    // Shifting towards the most significant end moves bytes towards index 0.
    const size_t byte_shift = shift / 8;
    const unsigned bit_shift = static_cast<unsigned>(shift % 8);
    Bytes shifted{};
    for (size_t position = 0; position + byte_shift < Work::SIZE; ++position) {
        const size_t source = position + byte_shift;
        unsigned word = static_cast<unsigned>(value[source]) << bit_shift;
        if (bit_shift != 0 && source + 1 < Work::SIZE) {
            word |= static_cast<unsigned>(value[source + 1]) >> (8U - bit_shift);
        }
        shifted[position] = static_cast<uint8_t>(word & 0xFFU);
    }
    value = shifted;
}

/// `minuend -= subtrahend`, which callers only reach when the result is non-negative.
void Subtract(Bytes& minuend, const Bytes& subtrahend) noexcept {
    unsigned borrow = 0;
    for (size_t position = Work::SIZE; position-- > 0;) {
        const unsigned left = minuend[position];
        const unsigned right = static_cast<unsigned>(subtrahend[position]) + borrow;
        borrow = left < right ? 1U : 0U;
        minuend[position] = static_cast<uint8_t>((left + (borrow * 256U)) - right);
    }
}

}  // namespace

Work Work::OfTarget(const Target& target) noexcept {
    // A zero target is not a difficulty this can invert. It is rejected by
    // `CompactToTarget` before a header carrying it is ever indexed; zero here is the
    // answer that keeps a caller which skipped that check from crediting anything.
    if (target == Bytes{}) {
        return Work{};
    }

    // divisor = target + 1. The addition can carry out of the top only when every byte
    // is 0xFF, and then the answer is exactly 2^256 / 2^256.
    Bytes divisor = target;
    bool carried = true;
    for (size_t position = Work::SIZE; carried && position-- > 0;) {
        carried = divisor[position] == 0xFF;
        divisor[position] = static_cast<uint8_t>(divisor[position] + 1);
    }
    if (carried) {
        return Work::FromU64(1);
    }

    // dividend = 2^256 − 1 − target, the bitwise complement. Since
    // dividend + divisor == 2^256 exactly,
    //
    //     floor(2^256 / divisor) == floor(dividend / divisor) + 1
    //
    // which is how the whole calculation stays inside 256 bits: 2^256 itself does not
    // fit, and widening to 512 bits to hold a value that cancels immediately would be
    // more code with more to get wrong.
    Bytes dividend = target;
    for (uint8_t& byte : dividend) {
        byte = static_cast<uint8_t>(~byte);
    }

    // Restoring binary long division, most significant quotient bit first. The divisor
    // is aligned under the dividend's highest set bit, so every shifted divisor has at
    // most as many bits as the dividend and therefore still fits: there is no
    // intermediate here that can overflow, which is why no wider type is needed.
    Bytes quotient{};
    Bytes remainder = dividend;
    const size_t dividend_bits = BitLength(dividend);
    const size_t divisor_bits = BitLength(divisor);
    if (dividend_bits >= divisor_bits) {
        for (size_t shift = dividend_bits - divisor_bits + 1; shift-- > 0;) {
            Bytes aligned = divisor;
            ShiftLeft(aligned, shift);
            if (remainder >= aligned) {
                Subtract(remainder, aligned);
                SetBit(quotient, shift);
            }
        }
    }

    // The + 1 from the identity above. It cannot carry out of the top: the target is at
    // least 1, so the divisor is at least 2 and the quotient at most 2^255 − 1.
    return Work::FromBigEndian(quotient) + Work::FromU64(1);
}

Work Work::OfCompactTarget(uint32_t bits) noexcept {
    const std::optional<Target> target = CompactToTarget(bits);
    if (!target.has_value()) {
        return Work{};
    }
    return OfTarget(*target);
}

Work& Work::operator+=(const Work& other) noexcept {
    unsigned carry = 0;
    for (size_t position = SIZE; position-- > 0;) {
        const unsigned sum = static_cast<unsigned>(bytes_[position]) +
                             static_cast<unsigned>(other.bytes_[position]) + carry;
        bytes_[position] = static_cast<uint8_t>(sum & 0xFFU);
        carry = sum >> 8U;
    }
    if (carry != 0) {
        bytes_.fill(0xFF);
    }
    return *this;
}

std::string Work::ToHex() const {
    return amarian::ToHex(ByteSpan{bytes_});
}

}  // namespace amarian
