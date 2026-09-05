#include <amarian/mempool.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace amarian {
namespace {

/// An "unknown" lock version (>0), which consensus treats as spendable by any
/// witness (the soft-fork policy). Same trick the utxo tests use to exercise the
/// real validation path without a signing dependency.
constexpr uint8_t FUTURE_LOCK_VERSION = 200;
/// An "unknown" spend-condition version: `CheckSpendCondition` accepts it (only
/// version 0 is reserved), and `CheckWitness` skips the threshold/signature-count
/// and signature-size checks that only apply to the known version 1. With an
/// unknown lock version the authorisation check is skipped entirely, so no
/// signatures are needed.
constexpr uint8_t FUTURE_CONDITION_VERSION = 200;

[[nodiscard]] Lock SpendableLock() {
    return Lock{.version = FUTURE_LOCK_VERSION, .program = ByteVec(4, 0x11)};
}

[[nodiscard]] Witness SpendableWitness() {
    Witness w;
    w.condition.version = FUTURE_CONDITION_VERSION;
    return w;
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

[[nodiscard]] OutPoint Op(uint8_t seed) {
    std::array<uint8_t, Hash256::SIZE> bytes{};
    bytes[0] = seed;
    return OutPoint{.txid = Hash256{bytes}, .index = 0};
}

/// No coins at all, spelled out rather than written `{}`: with two `Accept` overloads
/// an empty braced list is ambiguous between a span and a `CoinsView`.
const std::span<const Coin> NO_COINS{};

/// The reason a rejection carries, for an assertion that would otherwise print an
/// integer. Fails the calling test if the pool actually accepted.
[[nodiscard]] mempool::RejectReason
ReasonOf(const std::expected<const mempool::Entry*, mempool::Rejection>& result) {
    if (result.has_value()) {
        ADD_FAILURE() << "the pool accepted a transaction it should have refused";
        return mempool::RejectReason::AlreadyPresent;
    }
    return result.error().reason;
}

TEST(Mempool, RejectsCoinbase) {
    mempool::Mempool pool;
    Transaction tx;
    tx.version = 1;
    tx.inputs = {MakeCoinbaseInput(1)};
    tx.outputs = {TxOutput{.amount = 100, .lock = SpendableLock()}};
    EXPECT_EQ(ReasonOf(pool.Accept(tx, NO_COINS, 2, 1000000, REGTEST_PARAMS)),
              mempool::RejectReason::Coinbase);
}

TEST(Mempool, RejectsEmptyTransaction) {
    mempool::Mempool pool;
    Transaction tx;
    tx.version = 1;
    const auto result = pool.Accept(tx, NO_COINS, 2, 1000000, REGTEST_PARAMS);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().reason, mempool::RejectReason::ConsensusRuleFailed);
    // The consensus rule that refused it travels with the policy reason, so a log or an
    // RPC can say which one without the pool having flattened the two vocabularies.
    ASSERT_TRUE(result.error().rule.has_value());
}

TEST(Mempool, AcceptsValidSpendAndTracksIt) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction tx = MakeSpend(Op(1), 900'000);  // fee 100000
    const auto result = pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS);
    ASSERT_TRUE(result.has_value()) << mempool::Describe(result.error().reason);
    EXPECT_EQ(pool.Size(), size_t{1});
    EXPECT_GT(pool.TotalWeight(), size_t{0});
    EXPECT_EQ((*result)->fee, int64_t{100'000});
    EXPECT_EQ((*result)->txid, tx.Txid());
    EXPECT_EQ((*result)->wtxid, tx.Wtxid());
    EXPECT_EQ(pool.TotalFees(), int64_t{100'000});
    EXPECT_EQ(pool.Find(tx.Wtxid()), *result);
    EXPECT_EQ(pool.FindByTxid(tx.Txid()), *result);
    EXPECT_EQ(pool.SpenderOf(Op(1)), *result);
}

TEST(Mempool, RejectsZeroFee) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction tx = MakeSpend(Op(2), 1'000'000);  // zero fee
    EXPECT_EQ(ReasonOf(pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS)),
              mempool::RejectReason::FeeBelowRelayMinimum);
}

TEST(Mempool, RejectsMinting) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction tx = MakeSpend(Op(3), 2'000'000);  // outputs > inputs
    EXPECT_EQ(ReasonOf(pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS)),
              mempool::RejectReason::ConsensusRuleFailed);
}

TEST(Mempool, RemovesEntry) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction tx = MakeSpend(Op(4), 900'000);
    ASSERT_TRUE(pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS).has_value());
    EXPECT_EQ(pool.Size(), size_t{1});
    pool.Remove(tx.Wtxid());
    EXPECT_EQ(pool.Size(), size_t{0});
    EXPECT_EQ(pool.TotalWeight(), size_t{0});
    EXPECT_EQ(pool.TotalFees(), int64_t{0});
    // Every index the entry was in, not just the map it was keyed by.
    EXPECT_EQ(pool.FindByTxid(tx.Txid()), nullptr);
    EXPECT_EQ(pool.SpenderOf(Op(4)), nullptr);
}

TEST(Mempool, GetTemplatesReturnsAcceptedTx) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction tx = MakeSpend(Op(5), 900'000);
    ASSERT_TRUE(pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS).has_value());
    const auto templates = pool.GetTemplates(2'000'000);
    ASSERT_EQ(templates.size(), size_t{1});
    EXPECT_EQ(templates[0]->wtxid, tx.Wtxid());
    EXPECT_EQ(templates[0]->fee, int64_t{100'000});
}

TEST(Mempool, GetTemplatesRespectsWeightBudget) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction tx = MakeSpend(Op(6), 900'000);
    ASSERT_TRUE(pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS).has_value());
    // Offer less weight than the transaction needs.
    const auto templates = pool.GetTemplates(10);
    EXPECT_EQ(templates.size(), size_t{0});
}

TEST(Mempool, DuplicateWtxidIsRejected) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction tx = MakeSpend(Op(7), 900'000);
    ASSERT_TRUE(pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS).has_value());
    EXPECT_EQ(ReasonOf(pool.Accept(tx, coins, 22, 1000000, REGTEST_PARAMS)),
              mempool::RejectReason::AlreadyPresent);
}

/// A child spending a parent that is only in the pool, which is the case the
/// `CoinsView` overload exists for: without it a chain of unconfirmed transactions
/// cannot form, and the child-pays-for-parent ranking has nothing to rank.
TEST(Mempool, AcceptsChildSpendingInPoolParent) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction parent = MakeSpend(Op(8), 900'000);
    ASSERT_TRUE(pool.Accept(parent, coins, 22, 1000000, REGTEST_PARAMS).has_value());

    const utxo::EmptyCoinsView empty;
    const Transaction child = MakeSpend(OutPoint{.txid = parent.Txid(), .index = 0}, 800'000);
    const auto accepted = pool.Accept(child, empty, 23, 1000001, REGTEST_PARAMS);
    ASSERT_TRUE(accepted.has_value()) << mempool::Describe(accepted.error().reason);
    EXPECT_EQ(pool.Size(), size_t{2});

    // Parent ahead of child: a block in the other order would be invalid.
    const auto templates = pool.GetTemplates(2'000'000);
    ASSERT_EQ(templates.size(), size_t{2});
    EXPECT_EQ(templates[0]->wtxid, parent.Wtxid());
    EXPECT_EQ(templates[1]->wtxid, child.Wtxid());
}

/// Discarding a parent must discard the child too: a child whose parent is neither
/// confirmed nor in the pool can never be mined, and one left behind would be offered
/// in every template from then on.
TEST(Mempool, RemoveRecursiveTakesDescendants) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction parent = MakeSpend(Op(9), 900'000);
    ASSERT_TRUE(pool.Accept(parent, coins, 22, 1000000, REGTEST_PARAMS).has_value());
    const utxo::EmptyCoinsView empty;
    const Transaction child = MakeSpend(OutPoint{.txid = parent.Txid(), .index = 0}, 800'000);
    ASSERT_TRUE(pool.Accept(child, empty, 23, 1000001, REGTEST_PARAMS).has_value());

    EXPECT_EQ(pool.RemoveRecursive(parent.Wtxid()), size_t{2});
    EXPECT_EQ(pool.Size(), size_t{0});
    EXPECT_EQ(pool.TotalWeight(), size_t{0});
}

/// A connected block both confirms and invalidates. Without this the pool re-offers
/// mined transactions forever and every later template is invalid.
TEST(Mempool, RemoveForBlockDropsMinedAndConflicting) {
    mempool::Mempool pool;
    const std::vector<Coin> coins{MakeCoin(1'000'000, 1, false)};
    const Transaction mined = MakeSpend(Op(10), 900'000);
    ASSERT_TRUE(pool.Accept(mined, coins, 22, 1000000, REGTEST_PARAMS).has_value());

    // A second transaction spending a different coin, which the block also spends by a
    // transaction the pool has never seen. It can never confirm, so it must go.
    const std::vector<Coin> other_coins{MakeCoin(1'000'000, 1, false)};
    const Transaction doomed = MakeSpend(Op(11), 500'000);
    ASSERT_TRUE(pool.Accept(doomed, other_coins, 22, 1000000, REGTEST_PARAMS).has_value());
    ASSERT_EQ(pool.Size(), size_t{2});

    Block block;
    block.transactions.push_back(mined);
    // Spends the same outpoint as `doomed`, paying a different amount, so it is a
    // different transaction with the same conflict.
    block.transactions.push_back(MakeSpend(Op(11), 400'000));

    EXPECT_EQ(pool.RemoveForBlock(block), size_t{2});
    EXPECT_EQ(pool.Size(), size_t{0});
}

}  // namespace
}  // namespace amarian
