#include <amarian/primitives/merkle.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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

TEST(Merkle, CountIsCommitted) {
    const auto one = ComputeMerkleRoot(TestIds(1));
    const auto two = ComputeMerkleRoot(TestIds(2));
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(two.has_value());
    EXPECT_NE(*one, *two);
}

}  // namespace
}  // namespace amarian
