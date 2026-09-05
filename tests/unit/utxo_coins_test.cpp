/// \file
/// The UTXO cache's mechanics: what a layer holds, what it hides, and what it flushes.
///
/// These are the invariants everything above depends on. `utxo_connect_test.cpp` covers
/// applying a block; this file covers the container that makes doing so reversible.

#include <amarian/utxo/coins.hpp>

#include <gtest/gtest.h>

#include <optional>

namespace amarian::utxo {
namespace {

[[nodiscard]] OutPoint Point(uint8_t seed, uint32_t index = 0) {
    Hash256 txid;
    txid.Array()[0] = seed;
    return OutPoint{.txid = txid, .index = index};
}

[[nodiscard]] Coin CoinOf(int64_t amount, uint32_t height = 1, bool is_coinbase = false) {
    return Coin{.output = TxOutput{.amount = amount,
                                   .lock = Lock{.version = LOCK_VERSION_CONDITION_COMMITMENT,
                                                .program = ByteVec(32, 0xAB)}},
                .height = height,
                .is_coinbase = is_coinbase};
}

TEST(CoinsCache, AnEmptyViewHasNothingAndACacheOverItStartsEmpty) {
    const EmptyCoinsView base;
    const CoinsCache cache = CoinsCache::Over(base);

    EXPECT_FALSE(base.HaveCoin(Point(1)));
    EXPECT_FALSE(cache.HaveCoin(Point(1)));
    EXPECT_FALSE(cache.GetCoin(Point(1)).has_value());
    EXPECT_EQ(cache.ChangeCount(), 0U);
}

TEST(CoinsCache, AddThenSpendLeavesNoTombstoneWhenTheBaseNeverHadIt) {
    // The property that keeps the in-memory UTXO set proportional to the unspent output
    // count rather than to the chain's whole history. A coin created and spent without the
    // base ever seeing it must leave the cache exactly as it found it.
    const EmptyCoinsView base;
    CoinsCache cache = CoinsCache::Over(base);

    ASSERT_TRUE(cache.AddCoin(Point(1), CoinOf(500)));
    EXPECT_EQ(cache.ChangeCount(), 1U);

    const std::optional<Coin> spent = cache.SpendCoin(Point(1));
    ASSERT_TRUE(spent.has_value());
    EXPECT_EQ(spent->output.amount, 500);
    EXPECT_EQ(cache.ChangeCount(), 0U);
    EXPECT_FALSE(cache.HaveCoin(Point(1)));
}

TEST(CoinsCache, SpendingACoinTheBaseHoldsLeavesATombstoneThatHidesIt) {
    // The other half of the same rule: here the entry is load-bearing, because without it
    // a read would fall through to the base and find the coin again.
    const EmptyCoinsView empty;
    CoinsCache root = CoinsCache::Over(empty);
    ASSERT_TRUE(root.AddCoin(Point(2), CoinOf(700)));

    CoinsCache layer = CoinsCache::Over(root);
    const std::optional<Coin> spent = layer.SpendCoin(Point(2));
    ASSERT_TRUE(spent.has_value());
    EXPECT_EQ(spent->output.amount, 700);
    EXPECT_EQ(layer.ChangeCount(), 1U);
    EXPECT_FALSE(layer.HaveCoin(Point(2)));
    // The base is untouched until the layer is flushed. This is what makes a rejected
    // block cost nothing.
    EXPECT_TRUE(root.HaveCoin(Point(2)));
}

TEST(CoinsCache, DiscardingALayerLeavesTheBaseExactlyAsItWas) {
    const EmptyCoinsView empty;
    CoinsCache root = CoinsCache::Over(empty);
    ASSERT_TRUE(root.AddCoin(Point(3), CoinOf(900)));

    CoinsCache layer = CoinsCache::Over(root);
    EXPECT_TRUE(layer.SpendCoin(Point(3)).has_value());
    ASSERT_TRUE(layer.AddCoin(Point(4), CoinOf(100)));
    layer.Discard();

    EXPECT_EQ(layer.ChangeCount(), 0U);
    EXPECT_TRUE(root.HaveCoin(Point(3)));
    EXPECT_FALSE(root.HaveCoin(Point(4)));
}

TEST(CoinsCache, FlushingMovesEveryChangeIntoTheBaseAndEmptiesTheLayer) {
    const EmptyCoinsView empty;
    CoinsCache root = CoinsCache::Over(empty);
    ASSERT_TRUE(root.AddCoin(Point(5), CoinOf(1'000)));

    CoinsCache layer = CoinsCache::Over(root);
    EXPECT_TRUE(layer.SpendCoin(Point(5)).has_value());
    ASSERT_TRUE(layer.AddCoin(Point(6), CoinOf(250, 7, true)));
    layer.Flush(root);

    EXPECT_EQ(layer.ChangeCount(), 0U);
    EXPECT_FALSE(root.HaveCoin(Point(5)));
    const std::optional<Coin> moved = root.GetCoin(Point(6));
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(moved->output.amount, 250);
    EXPECT_EQ(moved->height, 7U);
    EXPECT_TRUE(moved->is_coinbase);
    // The tombstone that hid the spent coin is gone rather than inherited: the root's base
    // is the empty view, which never had it.
    EXPECT_EQ(root.ChangeCount(), 1U);
}

TEST(CoinsCache, AddingOverALiveCoinFailsAndChangesNothing) {
    // A silent overwrite destroys coins, so the set refuses. `ConnectBlock` turns this into
    // `TxCreatesExistingOutpoint`.
    const EmptyCoinsView base;
    CoinsCache cache = CoinsCache::Over(base);
    ASSERT_TRUE(cache.AddCoin(Point(7), CoinOf(400)));

    EXPECT_FALSE(cache.AddCoin(Point(7), CoinOf(999)));
    const std::optional<Coin> held = cache.GetCoin(Point(7));
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(held->output.amount, 400);
}

TEST(CoinsCache, SpendingWhatIsNotThereReportsNothingAndAddsNoEntry) {
    const EmptyCoinsView base;
    CoinsCache cache = CoinsCache::Over(base);

    EXPECT_FALSE(cache.SpendCoin(Point(8)).has_value());
    EXPECT_EQ(cache.ChangeCount(), 0U);
}

TEST(CoinsCache, ACoinRecreatedAtAnOutpointItAlreadyOccupiedIsAllowedOnceSpent) {
    // Not a duplicate-outpoint violation: the outpoint is not in the set at the moment of
    // creation, so nothing is overwritten. Amarian makes the case unreachable in a real
    // chain — a coinbase txid carries its height, and a duplicate non-coinbase txid would
    // have to re-spend consumed outpoints — but the container's rule is about live coins,
    // and this pins down which rule it is.
    const EmptyCoinsView empty;
    CoinsCache root = CoinsCache::Over(empty);
    ASSERT_TRUE(root.AddCoin(Point(9), CoinOf(300)));

    CoinsCache layer = CoinsCache::Over(root);
    EXPECT_TRUE(layer.SpendCoin(Point(9)).has_value());
    EXPECT_TRUE(layer.AddCoin(Point(9), CoinOf(600)));
    const std::optional<Coin> held = layer.GetCoin(Point(9));
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(held->output.amount, 600);
}

}  // namespace
}  // namespace amarian::utxo
