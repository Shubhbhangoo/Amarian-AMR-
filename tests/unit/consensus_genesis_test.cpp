#include <amarian/consensus/genesis.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace amarian {
namespace {

/// The one check that has to hold for a node to start at all: each network's recorded
/// genesis hash is what its parameters actually build, the hash is real work against
/// the recorded target, and the block has the shape genesis is defined to have. A
/// build that failed this would not fork — it would agree with nobody about block 0.
TEST(Genesis, EveryNetworkMatchesItsRecordedHash) {
    for (const ChainParams& params : {MAINNET_PARAMS, TESTNET_PARAMS, REGTEST_PARAMS}) {
        EXPECT_EQ(CheckGenesis(params), GenesisFault::None) << params.name;
    }
}

/// A check that always passes is worse than no check, so this establishes that the
/// comparison is against the recorded constant and not against itself.
TEST(Genesis, TheCheckNoticesAChangedParameter) {
    ChainParams tampered = REGTEST_PARAMS;
    tampered.genesis_nonce += 1;
    EXPECT_EQ(CheckGenesis(tampered), GenesisFault::HashMismatch);

    tampered = REGTEST_PARAMS;
    tampered.genesis_timestamp += 1;
    EXPECT_EQ(CheckGenesis(tampered), GenesisFault::HashMismatch);

    tampered = REGTEST_PARAMS;
    tampered.genesis_hash = Hash256{};
    EXPECT_EQ(CheckGenesis(tampered), GenesisFault::HashMismatch);
}

/// There is no premine, and this is where that stops being a claim. Zero facets, and
/// a lock version that no witness can satisfy — not a burn address someone might hold
/// a key to.
TEST(Genesis, PaysNothingAndCannotBeSpent) {
    for (const ChainParams& params : {MAINNET_PARAMS, TESTNET_PARAMS, REGTEST_PARAMS}) {
        const Block genesis = BuildGenesisBlock(params);
        ASSERT_EQ(genesis.transactions.size(), 1U) << params.name;
        const Transaction& coinbase = genesis.transactions.front();

        EXPECT_TRUE(coinbase.IsCoinbase()) << params.name;
        ASSERT_EQ(coinbase.outputs.size(), 1U) << params.name;
        EXPECT_EQ(coinbase.outputs.front().amount, 0) << params.name;
        EXPECT_TRUE(coinbase.outputs.front().lock.IsUnspendable()) << params.name;

        // The unspendable lock's program is the network's chain id, which is what
        // makes this block belong to this network and to no other.
        const ByteVec chain_id(params.chain_id.Array().begin(), params.chain_id.Array().end());
        EXPECT_EQ(coinbase.outputs.front().lock.program, chain_id) << params.name;

        // The coinbase input commits to height 0 in `sequence`, per the rule that
        // keeps coinbase txids distinct.
        ASSERT_EQ(coinbase.inputs.size(), 1U) << params.name;
        EXPECT_EQ(coinbase.inputs.front().sequence, 0U) << params.name;
        EXPECT_EQ(genesis.header.height, 0U) << params.name;
        EXPECT_TRUE(genesis.header.prev_block.IsZero()) << params.name;
    }
}

/// Mainnet and testnet share every numeric parameter, so without something to
/// separate them their genesis blocks would differ only by a mined nonce and could
/// coincide. The chain id inside the coinbase is what keeps them apart, and it lands
/// in the Merkle root the header commits to.
TEST(Genesis, IsDistinctPerNetwork) {
    const Hash256 mainnet = BuildGenesisBlock(MAINNET_PARAMS).Hash();
    const Hash256 testnet = BuildGenesisBlock(TESTNET_PARAMS).Hash();
    const Hash256 regtest = BuildGenesisBlock(REGTEST_PARAMS).Hash();
    EXPECT_NE(mainnet, testnet);
    EXPECT_NE(mainnet, regtest);
    EXPECT_NE(testnet, regtest);

    EXPECT_NE(BuildGenesisBlock(MAINNET_PARAMS).header.merkle_root,
              BuildGenesisBlock(TESTNET_PARAMS).header.merkle_root);
}

/// Genesis has to survive the encoding every other block goes through, under the
/// bounds consensus actually uses. If the consensus limits rejected genesis, or if the
/// coinbase's byte-string witness section did not round-trip, no node could start.
TEST(Genesis, RoundTripsUnderConsensusLimits) {
    for (const ChainParams& params : {MAINNET_PARAMS, TESTNET_PARAMS, REGTEST_PARAMS}) {
        const Block genesis = BuildGenesisBlock(params);
        Writer writer;
        genesis.Serialize(writer);

        Reader reader(writer.Bytes());
        Block decoded;
        ASSERT_TRUE(Block::Deserialize(reader, decoded, params.block_limits)) << params.name;
        EXPECT_TRUE(reader.Finish()) << params.name;
        EXPECT_EQ(decoded, genesis) << params.name;
        EXPECT_EQ(decoded.Hash(), params.genesis_hash) << params.name;
        EXPECT_LE(genesis.Weight(), params.max_block_weight) << params.name;
    }
}

/// The coinbase bytes are the one field in the parameter table that is a claim about
/// the world rather than about arithmetic, so an edit to them should be visible.
TEST(Genesis, CarriesTheDatedPublicReference) {
    const Block genesis = BuildGenesisBlock(MAINNET_PARAMS);
    const ByteVec expected(GENESIS_MESSAGE.begin(), GENESIS_MESSAGE.end());
    EXPECT_EQ(genesis.transactions.front().coinbase_data, expected);
    EXPECT_LE(expected.size(), MAX_COINBASE_DATA_BYTES);
    EXPECT_NE(GENESIS_MESSAGE.find("05/Sep/2026"), std::string_view::npos);
}

}  // namespace
}  // namespace amarian
