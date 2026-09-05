#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/crypto/hash.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace amarian {
namespace {

/// The chain ids are literals in the header so that consensus reads a constant, but
/// they are only meaningful if they are the hash the specification says they are.
/// This recomputes the derivation through the crypto layer: a mistyped byte here
/// would otherwise create a network whose signatures no other implementation can
/// reproduce, and it would not show up until interoperability failed.
TEST(Params, ChainIdIsTheTaggedHashOfItsName) {
    for (const ChainParams& params : {MAINNET_PARAMS, TESTNET_PARAMS, REGTEST_PARAMS}) {
        const std::string_view name = params.name;
        const ByteVec message(name.begin(), name.end());
        EXPECT_EQ(params.chain_id, TaggedHash("Amarian/chain-id", message)) << name;
    }
}

TEST(Params, MagicIsTheChainIdPrefixWithTheTopBitSet) {
    for (const ChainParams& params : {MAINNET_PARAMS, TESTNET_PARAMS, REGTEST_PARAMS}) {
        const uint8_t* chain_id = params.chain_id.Data();
        EXPECT_EQ(params.magic[0], static_cast<uint8_t>(chain_id[0] | 0x80U)) << params.name;
        EXPECT_EQ(params.magic[1], chain_id[1]) << params.name;
        EXPECT_EQ(params.magic[2], chain_id[2]) << params.name;
        EXPECT_EQ(params.magic[3], chain_id[3]) << params.name;
    }
}

/// A network whose proof-of-work floor does not decode is a network that cannot
/// accept a block at all, and the failure would appear at genesis rather than here.
TEST(Params, EveryNetworkTargetDecodes) {
    for (const ChainParams& params : {MAINNET_PARAMS, TESTNET_PARAMS, REGTEST_PARAMS}) {
        EXPECT_TRUE(CompactToTarget(params.pow_limit_bits).has_value()) << params.name;
        EXPECT_TRUE(CompactToTarget(params.genesis_bits).has_value()) << params.name;

        // Genesis may be harder than the floor but never easier: a first block that
        // claimed a weaker target than the network permits would be unrejectable by
        // the same rule that is supposed to bound every later block.
        const auto floor_target = CompactToTarget(params.pow_limit_bits);
        const auto genesis_target = CompactToTarget(params.genesis_bits);
        ASSERT_TRUE(floor_target.has_value() && genesis_target.has_value());
        EXPECT_LE(*genesis_target, *floor_target) << params.name;
    }
}

/// Regtest's floor exists so that tests do not mine. This measures that claim rather
/// than asserting it: counting how many of a batch of distinct digests are work.
TEST(Params, RegtestWorkIsCheapAndMainnetWorkIsNot) {
    const auto regtest = CompactToTarget(REGTEST_PARAMS.pow_limit_bits);
    const auto mainnet = CompactToTarget(MAINNET_PARAMS.pow_limit_bits);
    ASSERT_TRUE(regtest.has_value() && mainnet.has_value());

    size_t regtest_hits = 0;
    size_t mainnet_hits = 0;
    for (uint32_t nonce = 0; nonce < 256; ++nonce) {
        ByteVec preimage{static_cast<uint8_t>(nonce), static_cast<uint8_t>(nonce >> 8)};
        const Hash256 hash = DoubleSha256(preimage);
        regtest_hits += HashMeetsTarget(hash, *regtest) ? 1U : 0U;
        mainnet_hits += HashMeetsTarget(hash, *mainnet) ? 1U : 0U;
    }
    // Roughly half at 2^255, and none at all at 2^224 out of 256 tries.
    EXPECT_GT(regtest_hits, 64U);
    EXPECT_EQ(mainnet_hits, 0U);
}

TEST(Params, LookupAndParsingAgree) {
    for (const ChainParams& params : {MAINNET_PARAMS, TESTNET_PARAMS, REGTEST_PARAMS}) {
        const auto parsed = NetworkFromName(params.name);
        ASSERT_TRUE(parsed.has_value()) << params.name;
        EXPECT_EQ(*parsed, params.network) << params.name;
        EXPECT_EQ(ParamsFor(*parsed).chain_id, params.chain_id) << params.name;
    }
    // Case matters, and nothing falls back to the network that holds real value.
    EXPECT_FALSE(NetworkFromName("Mainnet").has_value());
    EXPECT_FALSE(NetworkFromName("mainnet ").has_value());
}

}  // namespace
}  // namespace amarian
