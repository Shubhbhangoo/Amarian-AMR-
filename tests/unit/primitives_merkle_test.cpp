#include <amarian/primitives/merkle.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace amarian {
namespace {

std::vector<Hash256> TestIds(size_t count) {
    std::vector<Hash256> ids;
    ids.reserve(count);
    for (size_t value = 0; value < count; ++value) {
        ByteVec bytes(Hash256::SIZE, static_cast<uint8_t>(value));
        ids.push_back(Hash256::FromBytes(bytes));
    }
    return ids;
}

TEST(Merkle, EmptyTreeHasNoRoot) {
    EXPECT_FALSE(ComputeMerkleRoot({}).has_value());
}

TEST(Merkle, MatchesFixedVectors) {
    const auto one = ComputeMerkleRoot(TestIds(1));
    const auto two = ComputeMerkleRoot(TestIds(2));
    const auto three = ComputeMerkleRoot(TestIds(3));
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(two.has_value());
    ASSERT_TRUE(three.has_value());

    EXPECT_EQ(one->ToHexInternal(),
              "12ffa1211615a6a0f7e887ef8a281c8a160da0bf88da67b73cc3304db0211848");
    EXPECT_EQ(two->ToHexInternal(),
              "3bbf1c68180c0302be5e8d0a54f80bc487e9d6dfb999c8291068cadec45c929f");
    EXPECT_EQ(three->ToHexInternal(),
              "8bf1e0bcb8ba9c658be180d5428f342c3840a68f53080629300e6f41edb4e4a1");
}

TEST(Merkle, OddLeafIsPromotedRatherThanDuplicated) {
    const auto three = ComputeMerkleRoot(TestIds(3));
    auto four_ids = TestIds(3);
    four_ids.push_back(four_ids.back());
    const auto four = ComputeMerkleRoot(four_ids);
    ASSERT_TRUE(three.has_value());
    ASSERT_TRUE(four.has_value());
    EXPECT_NE(*three, *four);
}

// CVE-2012-2459 is not only a leaf-level bug. Under Bitcoin's rule a level of six
// leaves reduces to three inner nodes, the last of which is duplicated, so the
// eight-leaf list whose final pair repeats the preceding pair yields an identical
// root. The leaf-level test above would pass on an implementation that promoted
// leaves but still duplicated inner nodes, so cover the inner level explicitly.
TEST(Merkle, InnerLevelPromotionDoesNotCollide) {
    const auto six = ComputeMerkleRoot(TestIds(6));
    auto eight_ids = TestIds(6);
    eight_ids.push_back(eight_ids[4]);
    eight_ids.push_back(eight_ids[5]);
    const auto eight = ComputeMerkleRoot(eight_ids);
    ASSERT_TRUE(six.has_value());
    ASSERT_TRUE(eight.has_value());
    EXPECT_NE(*six, *eight);
}

// A one-transaction block's root is not its txid. The leaf tag and the count
// commitment both stand between them, so this fails if either is dropped —
// and an implementation where they coincide lets a bare txid be presented as a
// block's Merkle root.
TEST(Merkle, SingleLeafRootIsNotTheIdItself) {
    const auto ids = TestIds(1);
    const auto root = ComputeMerkleRoot(ids);
    ASSERT_TRUE(root.has_value());
    EXPECT_NE(*root, ids.front());
}

// Transaction order is part of what a block commits to: the coinbase is first by
// rule, and inputs may spend outputs created earlier in the same block. A tree
// that sorted or otherwise normalised its leaves would break both.
TEST(Merkle, LeafOrderIsCommitted) {
    const auto ids = TestIds(2);
    const std::vector<Hash256> reversed{ids[1], ids[0]};
    const auto forward = ComputeMerkleRoot(ids);
    const auto backward = ComputeMerkleRoot(reversed);
    ASSERT_TRUE(forward.has_value());
    ASSERT_TRUE(backward.has_value());
    EXPECT_NE(*forward, *backward);
}

TEST(Merkle, CountIsCommitted) {
    const auto one = ComputeMerkleRoot(TestIds(1));
    const auto two = ComputeMerkleRoot(TestIds(2));
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(two.has_value());
    EXPECT_NE(*one, *two);
}

// The property the three defences exist to provide, stated directly rather than
// through any one mechanism: distinct transaction lists have distinct roots.
// Every list of length 1..7 over three distinct ids is enumerated — 3279 of them,
// covering odd levels at every depth up to three — and all 3279 roots must
// differ. Unlike the tests above this is not aimed at a mutation anyone has
// thought of, which is the point: it fails for any future change that lets two
// blocks share a root, whatever the mechanism.
//
// scripts/mutate_merkle.sh records the limit of black-box testing here. Removing
// any single defence is caught only by MatchesFixedVectors, because the other two
// still hold — so the fixed vectors, cross-checked by the independent from-spec
// implementation in scripts/verify_vectors.py, are what pin each defence
// individually.
TEST(Merkle, DistinctListsHaveDistinctRoots) {
    constexpr size_t ALPHABET = 3;
    constexpr size_t MAX_LEN = 7;
    const std::vector<Hash256> symbols = TestIds(ALPHABET);

    std::set<std::string> roots;
    size_t lists = 0;
    for (size_t len = 1; len <= MAX_LEN; ++len) {
        std::vector<size_t> digits(len, 0);
        for (;;) {
            std::vector<Hash256> ids;
            ids.reserve(len);
            for (const size_t digit : digits) {
                ids.push_back(symbols[digit]);
            }
            const auto root = ComputeMerkleRoot(ids);
            ASSERT_TRUE(root.has_value());
            roots.insert(root->ToHexInternal());
            ++lists;

            size_t position = len;
            while (position > 0 && ++digits[position - 1] == ALPHABET) {
                digits[position - 1] = 0;
                --position;
            }
            if (position == 0) {
                break;
            }
        }
    }

    EXPECT_EQ(lists, 3279U);
    EXPECT_EQ(roots.size(), lists);
}

}  // namespace
}  // namespace amarian
