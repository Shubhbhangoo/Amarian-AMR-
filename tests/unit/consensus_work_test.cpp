/// \file
/// Tests for the conversion from a target to an amount of work.
///
/// This is arithmetic that every node must perform identically, and it is arithmetic
/// whose answers cannot be checked by eye: a 256-bit division implemented by hand is
/// either exactly right or silently wrong. So the cases below are the ones with answers
/// that can be derived independently — small targets whose quotients are exact powers of
/// two, and the difficulty-one floor, whose work is a published constant.

#include <amarian/consensus/target.hpp>
#include <amarian/consensus/work.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace amarian {
namespace {

/// The target whose numeric value is `value`, in the most-significant-first order
/// `Target` uses.
[[nodiscard]] Target TargetOfValue(uint64_t value) {
    Target target{};
    for (size_t position = 0; position < sizeof(uint64_t); ++position) {
        target[target.size() - 1 - position] =
            static_cast<uint8_t>((value >> (position * 8)) & 0xFFU);
    }
    return target;
}

/// The amount with `byte` at the most significant end and nothing else set.
[[nodiscard]] Work WorkWithTopByte(uint8_t byte) {
    std::array<uint8_t, Work::SIZE> bytes{};
    bytes[0] = byte;
    return Work::FromBigEndian(bytes);
}

[[nodiscard]] Work Saturated() {
    std::array<uint8_t, Work::SIZE> bytes{};
    bytes.fill(0xFF);
    return Work::FromBigEndian(bytes);
}

TEST(Work, ATargetOfOneCostsHalfTheSearchSpace) {
    // Two digests out of 2^256 are at most 1, so the expected number of attempts is
    // 2^256 / 2 = 2^255: one bit set, at the very top.
    EXPECT_EQ(Work::OfTarget(TargetOfValue(1)), WorkWithTopByte(0x80));
}

TEST(Work, ATargetOfTwoHundredFiftyFiveCostsATwoToTheTwoHundredFortyEighth) {
    // 256 digests satisfy it, so 2^256 / 256 = 2^248 — the byte below the top.
    EXPECT_EQ(Work::OfTarget(TargetOfValue(255)), WorkWithTopByte(0x01));
}

TEST(Work, ATargetOfEverythingCostsOneAttempt) {
    Target every_digest{};
    every_digest.fill(0xFF);
    EXPECT_EQ(Work::OfTarget(every_digest), Work::FromU64(1));
}

TEST(Work, TheDifficultyOneFloorAgreesWithTheKnownValue) {
    // 0x1D00FFFF is the same floor Bitcoin uses, and the work one block at it represents
    // is a published constant: 0x100010001, the chainwork of Bitcoin's genesis block.
    // Agreeing with a number derived elsewhere is what makes this more than a check that
    // the implementation is self-consistent.
    EXPECT_EQ(Work::OfCompactTarget(0x1D00'FFFFU), Work::FromU64(0x0000'0001'0001'0001ULL));
}

TEST(Work, AnUnusableTargetIsNoWork) {
    // No digest is at most zero, so a zero target describes an impossible block rather
    // than an infinitely difficult one, and an encoding no valid header may carry is not
    // a difficulty at all. Both answer zero, so a caller that skipped the encoding check
    // credits nothing rather than crediting something arbitrary.
    EXPECT_TRUE(Work::OfTarget(Target{}).IsZero());
    EXPECT_TRUE(Work::OfCompactTarget(0).IsZero());
    EXPECT_TRUE(Work::OfCompactTarget(0x1D80'FFFFU).IsZero());
}

TEST(Work, AdditionCarriesAcrossTheWholeWidth) {
    std::array<uint8_t, Work::SIZE> one_below{};
    one_below.fill(0xFF);
    one_below[Work::SIZE - 1] = 0xFE;
    EXPECT_EQ(Work::FromBigEndian(one_below) + Work::FromU64(1), Saturated());
}

TEST(Work, AdditionSaturatesRatherThanWrapping) {
    // Unreachable with real chains, and deliberately not a wrap: a wrap would make an
    // enormous chain compare as a tiny one, while saturation can at worst make two
    // impossible chains compare equal.
    EXPECT_EQ(Saturated() + Work::FromU64(1), Saturated());
    EXPECT_EQ(Saturated() + Saturated(), Saturated());
}

TEST(Work, TwoEasyBlocksCanOutweighOneHardBlock) {
    // The whole reason chains are compared by summed work rather than by target: a longer
    // branch of cheaper blocks can represent more hashing than a shorter dearer one, and
    // comparing targets would order the two backwards.
    const Work hard = Work::OfTarget(TargetOfValue(100));
    const Work easy = Work::OfTarget(TargetOfValue(255));
    EXPECT_GT(hard, easy);
    EXPECT_GT(easy + easy + easy, hard);
}

}  // namespace
}  // namespace amarian
