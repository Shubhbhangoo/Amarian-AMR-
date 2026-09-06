/// \file
/// Phase 9 regression tests: every attack class in THREAT_MODEL.md gets a test
/// that fails when the defence is removed.

#include <amarian/consensus/validation.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/primitives/spend_condition.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace amarian::consensus {
namespace {

// === 1. Supply integrity ===================================================

TEST(RegressionSupply, DuplicateCoinbaseRejected) {
    Block block;
    block.header.version = 1;
    block.header.height = 1;
    block.header.prev_block = REGTEST_PARAMS.genesis_hash;
    block.header.timestamp = REGTEST_PARAMS.genesis_timestamp + 300;
    block.header.target_bits = REGTEST_PARAMS.pow_limit_bits;

    Transaction coinbase1;
    coinbase1.version = 1;
    coinbase1.inputs.push_back(MakeCoinbaseInput(1));
    coinbase1.outputs.push_back(TxOutput{
        .amount = BlockReward(1, REGTEST_PARAMS.issuance),
        .lock = Lock{1, ByteVec(32, 0x11)},
    });
    block.transactions.push_back(coinbase1);

    Transaction coinbase2;
    coinbase2.version = 1;
    coinbase2.inputs.push_back(MakeCoinbaseInput(1));
    coinbase2.outputs.push_back(TxOutput{.amount = 1, .lock = Lock{1, ByteVec(32, 0x22)}});
    block.transactions.push_back(coinbase2);

    const Verdict result = CheckBlock(block, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ValidationError::BlockMultipleCoinbases);
}

TEST(RegressionSupply, CoinbaseMustBeFirstTransaction) {
    Block block;
    block.header.version = 1;
    block.header.height = 1;
    block.header.prev_block = REGTEST_PARAMS.genesis_hash;
    block.header.timestamp = REGTEST_PARAMS.genesis_timestamp + 300;
    block.header.target_bits = REGTEST_PARAMS.pow_limit_bits;

    Transaction noncoinbase;
    noncoinbase.version = 1;
    noncoinbase.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256{}, 0}, .sequence = 0});
    noncoinbase.outputs.push_back(TxOutput{.amount = 100, .lock = Lock{1, ByteVec(32, 0x11)}});
    noncoinbase.witnesses.push_back(Witness{});
    block.transactions.push_back(noncoinbase);

    Transaction coinbase;
    coinbase.version = 1;
    coinbase.inputs.push_back(MakeCoinbaseInput(1));
    coinbase.outputs.push_back(TxOutput{
        .amount = BlockReward(1, REGTEST_PARAMS.issuance),
        .lock = Lock{1, ByteVec(32, 0x11)},
    });
    block.transactions.push_back(coinbase);

    const Verdict result = CheckBlock(block, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ValidationError::BlockFirstTxNotCoinbase);
}

TEST(RegressionSupply, BlockMustHaveAtLeastOneTransaction) {
    Block block;
    block.header.version = 1;
    block.header.height = 1;
    block.header.prev_block = REGTEST_PARAMS.genesis_hash;
    block.header.timestamp = REGTEST_PARAMS.genesis_timestamp + 300;
    block.header.target_bits = REGTEST_PARAMS.pow_limit_bits;

    const Verdict result = CheckBlock(block, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ValidationError::BlockNoTransactions);
}

// === 2. Spend authorisation ================================================

TEST(RegressionSpend, UnspendableLockRejected) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256{}, 0}, .sequence = 0});
    tx.outputs.push_back(TxOutput{.amount = 100, .lock = Lock{1, ByteVec(32, 0xBB)}});
    tx.witnesses.push_back(Witness{});

    Coin coin;
    coin.output.amount = 100;
    coin.output.lock = Lock{0, ByteVec{}};
    coin.height = 1;
    coin.is_coinbase = false;

    const Verdict auth = CheckSpendAuthorisation(tx, std::span<const Coin>(&coin, 1),
                                                  REGTEST_PARAMS);
    EXPECT_FALSE(auth.has_value());
    EXPECT_EQ(auth.error(), ValidationError::TxSpendsUnspendableOutput);
}

TEST(RegressionSpend, TransactionNoInputs) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.outputs.push_back(TxOutput{.amount = 100, .lock = Lock{1, ByteVec(32, 0xBB)}});
    const Verdict result = CheckTransaction(tx, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
}

TEST(RegressionSpend, TransactionNoOutputs) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256{}, 0}, .sequence = 0});
    tx.witnesses.push_back(Witness{});
    const Verdict result = CheckTransaction(tx, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
}

TEST(RegressionSpend, NonCoinbaseNullOutpoint) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256{}, 1}, .sequence = 0});
    tx.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256{}, COINBASE_OUTPOINT_INDEX}, .sequence = 0});
    tx.outputs.push_back(TxOutput{.amount = 200, .lock = Lock{1, ByteVec(32, 0xBB)}});
    tx.witnesses.resize(2);
    const Verdict result = CheckTransaction(tx, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
}

// === 3. Resource exhaustion ================================================

TEST(RegressionResource, WeightTooLarge) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256{}, 0}, .sequence = 0});
    tx.witnesses.push_back(Witness{});
    for (size_t i = 0; i < 100000; ++i) {
        tx.outputs.push_back(TxOutput{.amount = 1, .lock = Lock{1, ByteVec(32, 0xBB)}});
    }
    const Verdict result = CheckTransaction(tx, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
}

// === 4. Determinism and convergence ========================================

TEST(RegressionDeterminism, DuplicateInput) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    const OutPoint op{Hash256{}, 0};
    tx.inputs.push_back(TxInput{.outpoint = op, .sequence = 0});
    tx.inputs.push_back(TxInput{.outpoint = op, .sequence = 1});
    tx.outputs.push_back(TxOutput{.amount = 200, .lock = Lock{1, ByteVec(32, 0xBB)}});
    tx.witnesses.resize(2);
    const Verdict result = CheckTransaction(tx, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
}

// === 5. Transaction structural rules =======================================

TEST(RegressionStructure, WitnessCountMismatch) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256{}, 0}, .sequence = 0});
    tx.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256{}, 1}, .sequence = 0});
    tx.outputs.push_back(TxOutput{.amount = 200, .lock = Lock{1, ByteVec(32, 0xBB)}});
    tx.witnesses.push_back(Witness{});
    const Verdict result = CheckTransaction(tx, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ValidationError::TxWitnessCountMismatch);
}

TEST(RegressionStructure, CoinbaseHasWitnesses) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs.push_back(MakeCoinbaseInput(1));
    tx.outputs.push_back(TxOutput{
        .amount = BlockReward(1, REGTEST_PARAMS.issuance),
        .lock = Lock{1, ByteVec(32, 0x11)},
    });
    tx.witnesses.push_back(Witness{});
    tx.coinbase_data = ByteVec(10, 0x00);
    const Verdict result = CheckTransaction(tx, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ValidationError::TxCoinbaseHasWitnesses);
}

TEST(RegressionStructure, CoinbaseDataTooLarge) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs.push_back(MakeCoinbaseInput(1));
    tx.outputs.push_back(TxOutput{
        .amount = BlockReward(1, REGTEST_PARAMS.issuance),
        .lock = Lock{1, ByteVec(32, 0x11)},
    });
    tx.coinbase_data = ByteVec(MAX_COINBASE_DATA_BYTES + 1, 0x00);
    const Verdict result = CheckTransaction(tx, REGTEST_PARAMS);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ValidationError::TxCoinbaseDataTooLarge);
}

// === 6. SpendCondition structural rules ====================================

TEST(RegressionCondition, ReservedVersion) {
    SpendCondition c;
    c.version = CONDITION_VERSION_RESERVED;
    c.threshold = 1;
    c.keys.resize(1);
    c.keys[0].scheme = 1;
    c.keys[0].bytes = ByteVec(32, 0xAA);
    EXPECT_FALSE(CheckSpendCondition(c, REGTEST_PARAMS).has_value());
}

TEST(RegressionCondition, NoKeys) {
    SpendCondition c;
    c.version = CONDITION_VERSION_THRESHOLD;
    c.threshold = 1;
    EXPECT_FALSE(CheckSpendCondition(c, REGTEST_PARAMS).has_value());
}

TEST(RegressionCondition, ThresholdZero) {
    SpendCondition c;
    c.version = CONDITION_VERSION_THRESHOLD;
    c.threshold = 0;
    c.keys.resize(1);
    c.keys[0].scheme = 1;
    c.keys[0].bytes = ByteVec(32, 0xAA);
    EXPECT_FALSE(CheckSpendCondition(c, REGTEST_PARAMS).has_value());
}

TEST(RegressionCondition, ThresholdAboveKeyCount) {
    SpendCondition c;
    c.version = CONDITION_VERSION_THRESHOLD;
    c.threshold = 2;
    c.keys.resize(1);
    c.keys[0].scheme = 1;
    c.keys[0].bytes = ByteVec(32, 0xAA);
    EXPECT_FALSE(CheckSpendCondition(c, REGTEST_PARAMS).has_value());
}

TEST(RegressionCondition, KeySchemeReserved) {
    SpendCondition c;
    c.version = CONDITION_VERSION_THRESHOLD;
    c.threshold = 1;
    c.keys.resize(1);
    c.keys[0].scheme = 0;
    EXPECT_FALSE(CheckSpendCondition(c, REGTEST_PARAMS).has_value());
}

TEST(RegressionCondition, UnsortedKeys) {
    SpendCondition c;
    c.version = CONDITION_VERSION_THRESHOLD;
    c.threshold = 2;
    c.keys.resize(2);
    c.keys[0].scheme = 2;
    c.keys[0].bytes = ByteVec(1312, 0xAA);
    c.keys[1].scheme = 1;
    c.keys[1].bytes = ByteVec(32, 0xBB);
    EXPECT_FALSE(CheckSpendCondition(c, REGTEST_PARAMS).has_value());
}

}  // namespace
}  // namespace amarian::consensus