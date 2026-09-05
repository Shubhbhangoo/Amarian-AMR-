#include <amarian/chain/block_index.hpp>
#include <amarian/consensus/issuance.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/mempool.hpp>
#include <amarian/mining/block_assembler.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/coins.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace amarian {
namespace {

constexpr uint8_t FUTURE_LOCK_VERSION = 200;
constexpr uint8_t FUTURE_CONDITION_VERSION = 200;

[[nodiscard]] Lock SpendableLock() {
    return Lock{.version = FUTURE_LOCK_VERSION, .program = ByteVec(4, 0x11)};
}

[[nodiscard]] Witness SpendableWitness() {
    Witness w;
    w.condition.version = FUTURE_CONDITION_VERSION;
    return w;
}

[[nodiscard]] OutPoint Op(uint8_t seed) {
    std::array<uint8_t, Hash256::SIZE> bytes{};
    bytes[0] = seed;
    return OutPoint{.txid = Hash256{bytes}, .index = 0};
}

[[nodiscard]] Coin MakeCoin(int64_t amount, uint32_t height, bool is_coinbase) {
    Coin coin;
    coin.output.amount = amount;
    coin.output.lock = SpendableLock();
    coin.height = height;
    coin.is_coinbase = is_coinbase;
    return coin;
}

[[nodiscard]] Transaction MakeSpend(const OutPoint& outpoint, int64_t paid) {
    Transaction tx;
    tx.version = 1;
    tx.inputs = {TxInput{.outpoint = outpoint, .sequence = 0}};
    tx.outputs = {TxOutput{.amount = paid, .lock = SpendableLock()}};
    tx.witnesses = {SpendableWitness()};
    return tx;
}

TEST(BuildBlockTemplate, IncludesCoinbaseOnlyWhenNoMempool) {
    chain::BlockIndex index = chain::BlockIndex::ForNetwork(REGTEST_PARAMS);
    const chain::BlockIndexEntry& genesis = index.Genesis();
    const Lock payout = SpendableLock();
    constexpr int64_t NOW = REGTEST_PARAMS.genesis_timestamp + 10;

    auto result = mining::BuildBlockTemplate(genesis, payout, NOW, {}, REGTEST_PARAMS);
    ASSERT_TRUE(result.has_value()) << mining::Describe(result.error());
    EXPECT_EQ(result->block.transactions.size(), size_t{1});
    EXPECT_TRUE(result->block.transactions[0].IsCoinbase());
    EXPECT_EQ(result->reward, BlockReward(1, REGTEST_PARAMS.issuance));
    EXPECT_EQ(result->fees, int64_t{0});
}

TEST(BuildBlockTemplate, RejectsOversizedCoinbaseData) {
    chain::BlockIndex index = chain::BlockIndex::ForNetwork(REGTEST_PARAMS);
    const chain::BlockIndexEntry& genesis = index.Genesis();
    const Lock payout = SpendableLock();
    constexpr int64_t NOW = REGTEST_PARAMS.genesis_timestamp + 10;

    ByteVec big(REGTEST_PARAMS.block_limits.tx.max_coinbase_data_size + 1, 0x42);
    auto result = mining::BuildBlockTemplate(genesis, payout, NOW, std::move(big),
                                             REGTEST_PARAMS);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), mining::TemplateError::CoinbaseDataTooLarge);
}

TEST(BuildBlockTemplate, TargetBitsFromNextTargetBits) {
    chain::BlockIndex index = chain::BlockIndex::ForNetwork(REGTEST_PARAMS);
    const chain::BlockIndexEntry& genesis = index.Genesis();
    const Lock payout = SpendableLock();
    constexpr int64_t NOW = REGTEST_PARAMS.genesis_timestamp + 10;

    auto result = mining::BuildBlockTemplate(genesis, payout, NOW, {}, REGTEST_PARAMS);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->block.header.target_bits,
              chain::NextTargetBits(genesis, REGTEST_PARAMS));
}

TEST(BuildBlockTemplate, CoinbasePaysRewardPlusFeesWhenMempoolHasTx) {
    chain::BlockIndex index = chain::BlockIndex::ForNetwork(REGTEST_PARAMS);
    const chain::BlockIndexEntry& genesis = index.Genesis();
    const Lock payout = SpendableLock();
    constexpr int64_t NOW = REGTEST_PARAMS.genesis_timestamp + 10;

    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction tx = MakeSpend(Op(1), 900'000);  // fee 100000
    ASSERT_TRUE(pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS).has_value());

    auto result =
        mining::BuildBlockTemplate(genesis, payout, NOW, {}, REGTEST_PARAMS, &pool);
    ASSERT_TRUE(result.has_value()) << mining::Describe(result.error());
    EXPECT_EQ(result->block.transactions.size(), size_t{2});
    EXPECT_TRUE(result->block.transactions[0].IsCoinbase());
    EXPECT_FALSE(result->block.transactions[1].IsCoinbase());
    EXPECT_EQ(result->block.transactions[1].Wtxid(), tx.Wtxid());
    const int64_t base_reward = BlockReward(1, REGTEST_PARAMS.issuance);
    EXPECT_EQ(result->reward, base_reward + 100000);
    EXPECT_EQ(result->fees, int64_t{100000});
}

TEST(SolveHeader, FindsNonceForTrivialTarget) {
    const std::optional<Target> target = CompactToTarget(REGTEST_PARAMS.pow_limit_bits);
    ASSERT_TRUE(target.has_value());
    BlockHeader header;
    header.version = 1;
    header.height = 1;
    header.timestamp = REGTEST_PARAMS.genesis_timestamp + 300;
    header.target_bits = REGTEST_PARAMS.pow_limit_bits;
    EXPECT_TRUE(mining::SolveHeader(header, *target, 10'000'000));
}

TEST(SolveHeader, ReturnsFalseWhenTargetUnreachable) {
    // An all-zero target is impossible to meet (a hash would have to be all zero),
    // so a small budget is exhausted without solving.
    Target impossible{};
    BlockHeader header;
    header.nonce = 0;
    EXPECT_FALSE(mining::SolveHeader(header, impossible, 5));
}

}  // namespace
}  // namespace amarian