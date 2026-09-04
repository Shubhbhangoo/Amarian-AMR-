#include <amarian/crypto/hash.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace amarian {
namespace {

TEST(CryptoHash, Sha256MatchesPublishedVectors) {
    EXPECT_EQ(Sha256(ByteSpan{}).ToHexInternal(),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const ByteVec abc{'a', 'b', 'c'};
    EXPECT_EQ(Sha256(abc).ToHexInternal(),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(CryptoHash, DoubleSha256MatchesPublishedVectors) {
    EXPECT_EQ(DoubleSha256(ByteSpan{}).ToHexInternal(),
              "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456");

    const ByteVec abc{'a', 'b', 'c'};
    EXPECT_EQ(DoubleSha256(abc).ToHexInternal(),
              "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");
}

TEST(CryptoHash, TaggedHashUsesTheBip340Construction) {
    const ByteVec message{0x00, 0x01, 0x02, 0x03};
    EXPECT_EQ(TaggedHash("Amarian/MerkleLeaf", message).ToHexInternal(),
              "c5d359e1f85809f86d88386fb2b0ee3b2191b77986ca5f5531918b579a725fcd");
}

TEST(CryptoHash, TagsDomainSeparateTheSameMessage) {
    const ByteVec message{0x00, 0x01, 0x02, 0x03};
    EXPECT_NE(TaggedHash("Amarian/MerkleLeaf", message),
              TaggedHash("Amarian/MerkleBranch", message));
    EXPECT_NE(TaggedHash("Amarian/MerkleLeaf", message), Sha256(message));
}

}  // namespace
}  // namespace amarian
