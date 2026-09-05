#pragma once

/// \file
/// Accumulated proof of work: the quantity that decides which chain is real.
///
/// A target says how hard a header was to find. Work says how much finding it cost.
/// The two are inverses, and the conversion matters because targets cannot be added: a
/// branch of two easy blocks and a branch of one hard block are compared by summing
/// work, and summing targets would order them backwards.
///
/// Work for a target *t* is the expected number of attempts needed to find a header at
/// or below it:
///
///     work(t) = floor(2^256 / (t + 1))
///
/// There are 2^256 possible digests and *t* + 1 of them satisfy the target, so a
/// uniformly distributed digest satisfies it with probability (*t* + 1) / 2^256 and the
/// expected number of tries is the reciprocal. The floor is deliberate: this is an
/// integer every node must compute identically, and a rounded consensus comparison is a
/// way for two honest nodes to disagree about a tip.
///
/// ## Why a type rather than an integer
///
/// `Work` supports what chain selection needs — construction from a target, addition,
/// and comparison — and nothing else. It cannot be multiplied, cannot be turned into a
/// floating-point number, and cannot be confused with a `Hash256` or a `Target` even
/// though all three are 256-bit values. Mixing those three up is the classic way a
/// proof-of-work implementation ends up disagreeing with every other one while still
/// appearing to work, so they are separate types with no conversions between them.
///
/// ## Byte order
///
/// Stored most significant byte first, the same orientation as `Target` and the
/// orientation `ToHex` prints. That is what makes the defaulted `==` and `<=>`
/// numerically correct: lexicographic order over big-endian bytes *is* numeric order,
/// so there is no hand-written comparison here to get wrong. Note this is the opposite
/// of `Hash256`'s internal order, which is one more reason no conversion exists.

#include <amarian/consensus/target.hpp>
#include <amarian/util/types.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>

namespace amarian {

/// A 256-bit amount of expected hashing, saturating on overflow.
class Work {
public:
    static constexpr size_t SIZE = 32;

    /// Zero: what genesis's absent predecessor has, and what an unusable target yields.
    constexpr Work() noexcept : bytes_{} {}

    /// Reads `bytes` as a big-endian 256-bit amount.
    ///
    /// For persistence, where accumulated work is stored beside a header, and for
    /// naming an expected amount in a test. Deliberately not a conversion from a hash
    /// or a target: both of those are different quantities that happen to be the same
    /// width.
    [[nodiscard]] static constexpr Work
    FromBigEndian(const std::array<uint8_t, SIZE>& bytes) noexcept {
        Work work;
        work.bytes_ = bytes;
        return work;
    }

    /// A small exact amount. The low end of the scale, where single attempts are a
    /// meaningful unit — regtest's target floor is high enough that a block costs a
    /// couple of them.
    [[nodiscard]] static constexpr Work FromU64(uint64_t value) noexcept {
        Work work;
        for (size_t position = 0; position < sizeof(uint64_t); ++position) {
            work.bytes_[SIZE - 1 - position] =
                static_cast<uint8_t>((value >> (position * 8)) & 0xFFU);
        }
        return work;
    }

    /// The work a header claiming `target` represents, or zero when `target` is zero.
    ///
    /// No digest is at most zero, so a zero target does not describe an infinitely
    /// difficult block: it describes an impossible one, which contributes nothing.
    [[nodiscard]] static Work OfTarget(const Target& target) noexcept;

    /// The work `bits` represents, or zero when `bits` is not a valid compact encoding.
    ///
    /// One function rather than two because an encoding no valid header may carry is
    /// not work either, so there is no way to accept the value and forget to check the
    /// encoding. `CheckBlockHeader` rejects such a header long before this is reached;
    /// this is the second answer to the same question, and they agree.
    [[nodiscard]] static Work OfCompactTarget(uint32_t bits) noexcept;

    [[nodiscard]] constexpr const std::array<uint8_t, SIZE>& BigEndian() const noexcept {
        return bytes_;
    }

    [[nodiscard]] constexpr bool IsZero() const noexcept { return *this == Work{}; }

    /// Adds, saturating at 2^256 − 1 rather than wrapping.
    ///
    /// Saturation cannot be reached: the total work of every chain that will ever exist
    /// is bounded by the number of hashes the physical universe permits, which is far
    /// below 2^256. It is chosen over wrapping because the two failure modes are not
    /// symmetric. A wrap would make an enormous chain compare as a tiny one, which is a
    /// chain-selection bug of the worst kind; saturation can at worst make two
    /// impossible chains compare equal, which the tie-break then resolves.
    Work& operator+=(const Work& other) noexcept;

    [[nodiscard]] friend Work operator+(Work first, const Work& second) noexcept {
        first += second;
        return first;
    }

    friend constexpr bool operator==(const Work&, const Work&) noexcept = default;

    /// Numeric order, by construction: see the byte-order note above.
    friend constexpr std::strong_ordering operator<=>(const Work& first,
                                                      const Work& second) noexcept {
        return first.bytes_ <=> second.bytes_;
    }

    /// Sixty-four hex characters, most significant first. Fixed width, so two printed
    /// amounts line up in a log and sort as text in the order they compare as numbers.
    [[nodiscard]] std::string ToHex() const;

private:
    std::array<uint8_t, SIZE> bytes_;
};

static_assert(sizeof(Work) == Work::SIZE, "Work must be a bare 32-byte amount");
static_assert(std::tuple_size_v<Target> == Work::SIZE,
              "a target and an amount of work must be the same width");

}  // namespace amarian
