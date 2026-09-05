#include <amarian/consensus/target.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace amarian {
namespace {

/// A digest with specific bytes set at specific internal indices; everything else
/// zero. Written in internal (digest) order deliberately, because the byte order
/// of the work comparison is exactly what these tests pin down.
Hash256 DigestWith(std::initializer_list<std::pair<size_t, uint8_t>> bytes) {
    ByteVec raw(Hash256::SIZE, 0);
    for (const auto& [index, value] : bytes) {
        raw[index] = value;
    }
    return Hash256::FromBytes(raw);
}

Target TargetWith(std::initializer_list<std::pair<size_t, uint8_t>> bytes) {
    Target target{};
    for (const auto& [index, value] : bytes) {
        target[index] = value;
    }
    return target;
}

TEST(Target, WorkIsComparedInDisplayOrder) {
    // The digest's *last* byte is the most significant, so a digest whose only
    // nonzero byte is at internal index 0 is a tiny number, and one whose only
    // nonzero byte is at index 31 is an enormous one. Getting this backwards
    // produces a chain that mines and validates and agrees with nobody.
    const Hash256 small = DigestWith({{0, 0xFF}});
    const Hash256 large = DigestWith({{Hash256::SIZE - 1, 0x01}});

    // 2^232: larger than `small` (255), smaller than `large` (2^248).
    const Target target = TargetWith({{2, 0x01}});

    EXPECT_TRUE(HashMeetsTarget(small, target));
    EXPECT_FALSE(HashMeetsTarget(large, target));
}

TEST(Target, EqualToTheTargetIsValidWork) {
    // The rule is "at most", so the boundary belongs to the miner.
    const Target target = TargetWith({{2, 0x01}});
    const Hash256 exactly = DigestWith({{Hash256::SIZE - 1 - 2, 0x01}});
    EXPECT_TRUE(HashMeetsTarget(exactly, target));

    const Hash256 one_more = DigestWith({{Hash256::SIZE - 1 - 2, 0x01}, {0, 0x01}});
    EXPECT_FALSE(HashMeetsTarget(one_more, target));
}

TEST(Target, RejectsTheSignBit) {
    // 0x00800000 is a sign a target cannot have, so no encoding may set it.
    EXPECT_FALSE(CompactToTarget(0x02800000U).has_value());
    EXPECT_FALSE(CompactToTarget(0x1D80FFFFU).has_value());
}

TEST(Target, RejectsAZeroTarget) {
    // No hash is at most zero, and zero is encodable at every exponent.
    EXPECT_FALSE(CompactToTarget(0x00000000U).has_value());
    EXPECT_FALSE(CompactToTarget(0x02000000U).has_value());
    EXPECT_FALSE(CompactToTarget(0x20000000U).has_value());
}

TEST(Target, RejectsNonCanonicalEncodingsRatherThanNormalisingThem) {
    // 0x1234 has exactly one valid encoding. The alternative spellings describe
    // the same target and are rejected, because two spellings are two headers.
    const uint32_t canonical = 0x02123400U;
    ASSERT_TRUE(CompactToTarget(canonical).has_value());

    EXPECT_FALSE(CompactToTarget(0x03001234U).has_value());
    EXPECT_FALSE(CompactToTarget(0x04000012U).has_value());
}

TEST(Target, RejectsMantissaBytesThatWouldBeTruncated) {
    // Exponent 1 leaves room for one mantissa byte; the other two would fall off
    // the bottom. Silently dropping them is how one target gains many encodings.
    EXPECT_FALSE(CompactToTarget(0x01123456U).has_value());
    EXPECT_TRUE(CompactToTarget(0x01120000U).has_value());
}

TEST(Target, RejectsValuesThatDoNotFitIn256Bits) {
    EXPECT_FALSE(CompactToTarget(0x21010000U).has_value());
    EXPECT_FALSE(CompactToTarget(0x227FFFFFU).has_value());
    EXPECT_FALSE(CompactToTarget(0xFF7FFFFFU).has_value());

    // The largest target that does fit.
    EXPECT_TRUE(CompactToTarget(0x207FFFFFU).has_value());
}

TEST(Target, CanonicalEncodingsSurviveARoundTrip) {
    // A single left-aligned mantissa byte at every exponent that fits.
    for (uint32_t exponent = 1; exponent <= 32; ++exponent) {
        const uint32_t bits = (exponent << 24) | 0x010000U;
        const auto target = CompactToTarget(bits);
        ASSERT_TRUE(target.has_value()) << "exponent " << exponent;
        EXPECT_EQ(TargetToCompact(*target), bits) << "exponent " << exponent;
    }
}

TEST(Target, AcceptsTheFamiliarBitcoinStyleEncoding) {
    // 0x1d00ffff is canonical under these rules too, which keeps existing
    // difficulty intuition and tooling transferable.
    const auto target = CompactToTarget(0x1D00FFFFU);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(TargetToCompact(*target), 0x1D00FFFFU);
    EXPECT_EQ((*target)[4], 0xFFU);
    EXPECT_EQ((*target)[5], 0xFFU);
    EXPECT_EQ((*target)[3], 0x00U);
}

TEST(Target, ZeroIsNotAnEncodableTarget) {
    EXPECT_EQ(TargetToCompact(Target{}), 0U);
}

TEST(Target, ProofOfWorkRequiresAUsableEncoding) {
    // An unusable encoding is not work. A hash of zero would satisfy any real
    // target, and must still fail when the target itself is invalid.
    const Hash256 zero;
    EXPECT_FALSE(CheckProofOfWork(zero, 0x02800000U));
    EXPECT_FALSE(CheckProofOfWork(zero, 0x00000000U));
    EXPECT_TRUE(CheckProofOfWork(zero, 0x1D00FFFFU));

    const Hash256 too_large = DigestWith({{Hash256::SIZE - 1, 0x01}});
    EXPECT_FALSE(CheckProofOfWork(too_large, 0x1D00FFFFU));
}

}  // namespace
}  // namespace amarian
