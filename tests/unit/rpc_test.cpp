/// \file
/// Tests for the typed JSON-RPC surface.
///
/// `rpc::Dispatch` is a function from (node, method, params) to JSON, and the reason
/// `amarian_rpc` contains no socket is so that the two claims that matter about it can be
/// made here, as function calls: that `getblocktemplate` describes exactly the block this
/// node would mine, and that `submitblock` runs a submitted block through exactly the
/// validation an arriving block gets. Both are checked below by solving a template this
/// node handed out and submitting it back — nothing binds a port, spawns a thread, or
/// authenticates.
///
/// The chain underneath is regtest with an in-memory store, which is what makes the round
/// trip cheap enough to be a unit test: regtest's floor is trivial, so a nonce search is a
/// handful of hashes rather than a proof of work.

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/block_store.hpp>
#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/mempool.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/rpc/dispatch.hpp>
#include <amarian/util/hex.hpp>
#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/coins.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace amarian {
namespace {

using nlohmann::json;

constexpr uint8_t FUTURE_LOCK_VERSION = 200;
constexpr int64_t NOW = REGTEST_PARAMS.genesis_timestamp + 600;

[[nodiscard]] Lock PayoutLock() {
    return Lock{.version = FUTURE_LOCK_VERSION, .program = ByteVec(4, 0x11)};
}

/// A whole node's worth of state, in memory: the index seeded with regtest genesis, an
/// empty coins set over it, and a store that keeps blocks in a map. `ChainState` seeds
/// genesis onto the chain itself, so the tip is valid the moment this is constructed.
struct Fixture {
    utxo::EmptyCoinsView base;
    chain::BlockIndex index = chain::BlockIndex::ForNetwork(REGTEST_PARAMS);
    utxo::CoinsCache coins = utxo::CoinsCache::Over(base);
    chain::MemoryBlockStore store;
    mempool::Mempool pool;
    chain::ChainState state{index, coins, store, REGTEST_PARAMS};
    rpc::Node node;

    Fixture() {
        node.state = &state;
        node.pool = &pool;
        node.params = &REGTEST_PARAMS;
        node.payout = PayoutLock();
    }
};

/// The block a template describes, decoded from the `block` field the miner is handed.
[[nodiscard]] Block BlockFromTemplate(const json& result) {
    const std::optional<ByteVec> bytes = FromHex(result.at("block").get<std::string>());
    EXPECT_TRUE(bytes.has_value());
    Block block;
    if (bytes.has_value()) {
        Reader reader(*bytes);
        EXPECT_TRUE(Block::Deserialize(reader, block, REGTEST_PARAMS.block_limits));
        EXPECT_TRUE(reader.Finish());
    }
    return block;
}

/// Searches the nonce for a header that meets its own target. Regtest's floor makes this
/// cheap; a failure to find one within the bound is a broken fixture, not a slow one.
[[nodiscard]] bool Solve(BlockHeader& header) {
    for (uint64_t nonce = 0; nonce < 1U << 20; ++nonce) {
        header.nonce = nonce;
        if (CheckProofOfWork(header.Hash(), header.target_bits)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string SerializedHex(const Block& block) {
    Writer writer;
    block.Serialize(writer);
    return ToHex(writer.Bytes());
}

TEST(Rpc, EveryAdvertisedMethodDispatchesAndNothingElseDoes) {
    // `help` lists what a client may call, and the table `Dispatch` scans is the same table
    // `MethodNames` reports: a name in one and not the other would be either a method no
    // client can discover or a promise the node cannot keep.
    Fixture fixture;
    for (const std::string_view name : rpc::MethodNames()) {
        const rpc::Result result = rpc::Dispatch(fixture.node, name, json::array(), NOW);
        if (!result.has_value()) {
            EXPECT_NE(result.error().code, rpc::RpcError::UnknownMethod)
                << "advertised method " << name << " is not dispatchable";
        }
    }

    const rpc::Result unknown =
        rpc::Dispatch(fixture.node, "not_a_method", json::array(), NOW);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code, rpc::RpcError::UnknownMethod);
}

TEST(Rpc, GetBlockTemplateNeedsSomewhereToPay) {
    // A node started without `--payout` cannot invent a destination for the subsidy, and
    // must say so rather than mine to a lock nobody holds.
    Fixture fixture;
    fixture.node.payout.reset();

    const rpc::Result result =
        rpc::Dispatch(fixture.node, "getblocktemplate", json::array(), NOW);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, rpc::RpcError::NoPayout);
}

TEST(Rpc, GetBlockTemplateDescribesTheBlockItHandsOut) {
    // The fields a miner reads must agree with the bytes it is given, because a miner that
    // trusts the fields and hashes the bytes would otherwise solve a different block than
    // the one the node described.
    Fixture fixture;
    const rpc::Result result =
        rpc::Dispatch(fixture.node, "getblocktemplate", json::array(), NOW);
    ASSERT_TRUE(result.has_value()) << rpc::Describe(result.error().code);

    const Block block = BlockFromTemplate(*result);
    EXPECT_EQ(result->at("height").get<uint32_t>(), block.header.height);
    EXPECT_EQ(result->at("height").get<uint32_t>(), fixture.state.Tip().height + 1);
    EXPECT_EQ(result->at("previous_block_hash").get<std::string>(),
              block.header.prev_block.ToHex());
    EXPECT_EQ(result->at("merkle_root").get<std::string>(), block.header.merkle_root.ToHex());
    EXPECT_EQ(result->at("timestamp").get<int64_t>(), block.header.timestamp);
    EXPECT_EQ(result->at("target_bits").get<uint32_t>(), block.header.target_bits);
    EXPECT_EQ(result->at("weight").get<size_t>(), block.Weight());

    // An empty pool means a coinbase and nothing else, and the subsidy is the whole of
    // `coinbase_value` because there are no fees to add to it.
    ASSERT_EQ(block.transactions.size(), size_t{1});
    EXPECT_TRUE(block.transactions[0].IsCoinbase());
    EXPECT_TRUE(result->at("transactions").empty());
    EXPECT_EQ(result->at("fees").get<int64_t>(), 0);
    EXPECT_EQ(result->at("coinbase_value").get<int64_t>(),
              block.transactions[0].outputs[0].amount);
}

TEST(Rpc, ASolvedTemplateSubmittedBackBecomesTheTip) {
    // The whole point of the pair: what `getblocktemplate` describes is a block this node
    // will accept, and `submitblock` puts it on the chain by the same path an arriving
    // block takes.
    Fixture fixture;
    const rpc::Result templated =
        rpc::Dispatch(fixture.node, "getblocktemplate", json::array(), NOW);
    ASSERT_TRUE(templated.has_value()) << rpc::Describe(templated.error().code);

    Block block = BlockFromTemplate(*templated);
    ASSERT_TRUE(Solve(block.header));

    const rpc::Result submitted = rpc::Dispatch(
        fixture.node, "submitblock", json::array({SerializedHex(block)}), NOW);
    ASSERT_TRUE(submitted.has_value()) << rpc::Describe(submitted.error().code);
    // Null is acceptance. Any string is a refusal, and its text is the reason.
    EXPECT_TRUE(submitted->is_null()) << submitted->dump();

    EXPECT_EQ(fixture.state.Tip().header.Hash(), block.header.Hash());
    EXPECT_EQ(fixture.state.Tip().height, 1U);
    EXPECT_TRUE(fixture.store.HaveBlock(block.header.Hash()));

    const rpc::Result info =
        rpc::Dispatch(fixture.node, "getblockchaininfo", json::array(), NOW);
    ASSERT_TRUE(info.has_value()) << rpc::Describe(info.error().code);
    EXPECT_EQ(info->at("blocks").get<uint32_t>(), 1U);

    // `1U` and not `1`: heights and counts are read with `is_number_unsigned`, so that a
    // negative one is a malformed request rather than a value to clamp. A JSON parser gives
    // a non-negative literal that type on its own; a test constructing the value in-process
    // has to ask for it.
    const rpc::Result at_one =
        rpc::Dispatch(fixture.node, "getblockhash", json::array({1U}), NOW);
    ASSERT_TRUE(at_one.has_value()) << rpc::Describe(at_one.error().code);
    EXPECT_EQ(at_one->get<std::string>(), block.header.Hash().ToHex());
}

TEST(Rpc, SubmitBlockSeparatesMalformedInputFromRejectedBlocks) {
    // Two different failures that a client must be able to tell apart: bytes this node
    // could not read, which is an error, and a block it read and refused, which is a
    // result. Neither may move the tip.
    Fixture fixture;
    const Hash256 tip_before = fixture.state.Tip().header.Hash();

    const rpc::Result not_hex =
        rpc::Dispatch(fixture.node, "submitblock", json::array({"zzzz"}), NOW);
    ASSERT_FALSE(not_hex.has_value());
    EXPECT_EQ(not_hex.error().code, rpc::RpcError::MalformedHex);

    const rpc::Result truncated =
        rpc::Dispatch(fixture.node, "submitblock", json::array({"00112233"}), NOW);
    ASSERT_FALSE(truncated.has_value());
    EXPECT_EQ(truncated.error().code, rpc::RpcError::MalformedBlock);

    const rpc::Result templated =
        rpc::Dispatch(fixture.node, "getblocktemplate", json::array(), NOW);
    ASSERT_TRUE(templated.has_value()) << rpc::Describe(templated.error().code);
    Block block = BlockFromTemplate(*templated);

    // A well-formed block with a byte glued to the end is not a block; accepting it would
    // let the same block arrive under two different serialisations.
    const rpc::Result trailing = rpc::Dispatch(
        fixture.node, "submitblock", json::array({SerializedHex(block) + "00"}), NOW);
    ASSERT_FALSE(trailing.has_value());
    EXPECT_EQ(trailing.error().code, rpc::RpcError::MalformedBlock);

    // Read and refused, which is a result with a reason in it rather than an error. The
    // defect is a merkle root that does not commit to the block's transactions; leaving the
    // nonce unsolved would not do, because regtest's floor is met by half of all hashes and
    // so an untouched template is already a valid solution about half the time.
    Block corrupt = block;
    corrupt.header.merkle_root = Hash256{};
    ASSERT_TRUE(Solve(corrupt.header));
    const rpc::Result refused = rpc::Dispatch(
        fixture.node, "submitblock", json::array({SerializedHex(corrupt)}), NOW);
    ASSERT_TRUE(refused.has_value()) << rpc::Describe(refused.error().code);
    ASSERT_TRUE(refused->is_string()) << refused->dump();
    EXPECT_FALSE(refused->get<std::string>().empty());

    EXPECT_EQ(fixture.state.Tip().header.Hash(), tip_before);
}

TEST(Rpc, GenerateMinesOnlyWhereDifficultyIsTrivial) {
    // `generate` exists for regtest, where a block costs a few hashes. Pointing it at
    // mainnet parameters would be a request to burn the caller's CPU inside an RPC handler,
    // so it is refused by the parameter set rather than by a flag someone could forget.
    Fixture fixture;
    const rpc::Result generated =
        rpc::Dispatch(fixture.node, "generate", json::array({2U}), NOW);
    ASSERT_TRUE(generated.has_value()) << rpc::Describe(generated.error().code);
    ASSERT_TRUE(generated->is_array()) << generated->dump();
    EXPECT_EQ(generated->size(), size_t{2});
    EXPECT_EQ(fixture.state.Tip().height, 2U);
    // The hashes come back in the order they were mined, tip last.
    EXPECT_EQ(generated->back().get<std::string>(), fixture.state.Tip().header.Hash().ToHex());

    fixture.node.params = &MAINNET_PARAMS;
    const rpc::Result denied =
        rpc::Dispatch(fixture.node, "generate", json::array({1U}), NOW);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code, rpc::RpcError::MiningNotPermitted);
}

}  // namespace
}  // namespace amarian
