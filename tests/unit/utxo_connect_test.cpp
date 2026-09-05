/// \file
/// Applying a block to the UTXO set, and taking it back off again.
///
/// The property everything above this layer depends on is that disconnecting restores
/// *exactly* what connecting removed, so that is what the central test here measures. The
/// rest cover the rules the set itself decides: an input must exist, a coinbase must have
/// matured, an outpoint may not be overwritten, and a rejected block must cost the set
/// nothing.
///
/// The coins here are locked with an unknown lock version, which consensus deliberately
/// treats as spendable by any witness so that a new lock form can be soft-forked in. That
/// keeps these tests on the real `ConnectBlock` path without a signing dependency — spend
/// authorisation has its own tests in `test_consensus`, against real ML-DSA signatures, and
/// duplicating them here would test the signature code twice and the set once.

#include <amarian/consensus/issuance.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/utxo/connect.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace amarian::utxo {
namespace {

using consensus::ValidationError;

constexpr uint8_t FUTURE_LOCK_VERSION = 200;

/// Regtest, for its 20-block coinbase maturity: long enough that immature and mature are
/// different heights, short enough that a test can build the gap in one line.
[[nodiscard]] const ChainParams& Params() noexcept {
    return REGTEST_PARAMS;
}

constexpr uint32_t FUNDING_HEIGHT = 1;
constexpr uint32_t MATURE_HEIGHT = FUNDING_HEIGHT + REGTEST_PARAMS.coinbase_maturity;

[[nodiscard]] Lock SpendableLock() {
    return Lock{.version = FUTURE_LOCK_VERSION, .program = ByteVec(4, 0x11)};
}

[[nodiscard]] Transaction MakeCoinbase(uint32_t height, const std::vector<int64_t>& amounts) {
    Transaction tx;
    tx.version = 1;
    tx.inputs = {MakeCoinbaseInput(height)};
    for (const int64_t amount : amounts) {
        tx.outputs.push_back(TxOutput{.amount = amount, .lock = SpendableLock()});
    }
    return tx;
}

[[nodiscard]] Transaction MakeSpend(const OutPoint& outpoint, int64_t paid) {
    Transaction tx;
    tx.version = 1;
    tx.inputs = {TxInput{.outpoint = outpoint, .sequence = 0}};
    tx.outputs = {TxOutput{.amount = paid, .lock = SpendableLock()}};
    tx.witnesses = {Witness{}};
    return tx;
}

[[nodiscard]] Block MakeBlock(uint32_t height, std::vector<Transaction> transactions) {
    Block block;
    block.header.version = 1;
    block.header.height = height;
    block.header.target_bits = Params().genesis_bits;
    block.transactions = std::move(transactions);
    return block;
}

[[nodiscard]] OutPoint FirstOutputOf(const Block& block) {
    return OutPoint{.txid = block.transactions.front().Txid(), .index = 0};
}

/// A block paying `amounts` from its coinbase at `FUNDING_HEIGHT`, already connected.
[[nodiscard]] Block Fund(CoinsCache& coins, const std::vector<int64_t>& amounts) {
    const Block funding = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, amounts)});
    const auto connected = ConnectBlock(funding, coins, Params());
    if (!connected.has_value()) {
        ADD_FAILURE() << consensus::Describe(connected.error());
    }
    return funding;
}

TEST(ConnectBlock, ACoinbasesOutputsEnterTheSetWithItsHeightAndFlag) {
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const Block block = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, {1'000, 2'000})});

    const auto result = ConnectBlock(block, coins, Params());
    ASSERT_TRUE(result.has_value()) << consensus::Describe(result.error());
    EXPECT_EQ(result->total_fees, 0);
    // No entry for the coinbase: it spends nothing, so there is nothing to undo.
    EXPECT_TRUE(result->undo.transactions.empty());

    const Hash256 txid = block.transactions.front().Txid();
    for (uint32_t index = 0; index < 2; ++index) {
        const std::optional<Coin> coin = coins.GetCoin(OutPoint{.txid = txid, .index = index});
        ASSERT_TRUE(coin.has_value()) << "output " << index;
        EXPECT_EQ(coin->height, FUNDING_HEIGHT);
        EXPECT_TRUE(coin->is_coinbase);
    }
    EXPECT_EQ(coins.ChangeCount(), 2U);
}

TEST(DisconnectBlock, RestoresTheSetExactlyAsItWas) {
    // The property a reorganisation stakes everything on. Not "a coin comes back" — the
    // same coin, with the same amount, lock, creation height and coinbase flag.
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const Block funding = Fund(coins, {1'000, 2'000});
    const OutPoint funded = FirstOutputOf(funding);

    const std::optional<Coin> before = coins.GetCoin(funded);
    ASSERT_TRUE(before.has_value());
    const size_t changes_before = coins.ChangeCount();

    const Block spending =
        MakeBlock(MATURE_HEIGHT, {MakeCoinbase(MATURE_HEIGHT, {500}), MakeSpend(funded, 600)});
    const auto connected = ConnectBlock(spending, coins, Params());
    ASSERT_TRUE(connected.has_value()) << consensus::Describe(connected.error());
    EXPECT_EQ(connected->total_fees, 400);
    EXPECT_FALSE(coins.HaveCoin(funded));
    ASSERT_EQ(connected->undo.transactions.size(), 1U);
    ASSERT_EQ(connected->undo.transactions.front().spent.size(), 1U);
    EXPECT_EQ(connected->undo.transactions.front().spent.front(), *before);

    const auto disconnected = DisconnectBlock(spending, connected->undo, coins);
    ASSERT_TRUE(disconnected.has_value()) << Describe(disconnected.error());

    const std::optional<Coin> after = coins.GetCoin(funded);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*after, *before);
    // And nothing else was left behind: the coins the block created are gone, and no
    // tombstone remains for the coin it spent.
    EXPECT_EQ(coins.ChangeCount(), changes_before);
    const Hash256 spending_coinbase = spending.transactions.front().Txid();
    EXPECT_FALSE(coins.HaveCoin(OutPoint{.txid = spending_coinbase, .index = 0}));
}

TEST(ConnectBlock, AnInputThatIsNotInTheSetIsRejectedAndTheSetIsUntouched) {
    // Also the atomicity property: the coinbase's outputs were already added to the batch
    // when the spend failed, and none of them reached the caller's set.
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const OutPoint absent{.txid = Hash256{}, .index = 3};
    const Block block =
        MakeBlock(MATURE_HEIGHT, {MakeCoinbase(MATURE_HEIGHT, {500}), MakeSpend(absent, 100)});

    const auto result = ConnectBlock(block, coins, Params());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ValidationError::TxInputMissingOrSpent);
    EXPECT_EQ(coins.ChangeCount(), 0U);
}

TEST(ConnectBlock, ACoinbaseOutputCannotBeSpentUntilItHasMatured) {
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const OutPoint funded = FirstOutputOf(Fund(coins, {1'000}));

    const uint32_t too_early_height = MATURE_HEIGHT - 1;
    const Block too_early = MakeBlock(
        too_early_height, {MakeCoinbase(too_early_height, {500}), MakeSpend(funded, 600)});
    const auto rejected = ConnectBlock(too_early, coins, Params());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::TxCoinbaseNotMature);

    // One block later it is spendable: a coin created at H is first spendable at
    // H + coinbase_maturity, and the boundary is checked rather than assumed.
    const Block just_in_time =
        MakeBlock(MATURE_HEIGHT, {MakeCoinbase(MATURE_HEIGHT, {500}), MakeSpend(funded, 600)});
    const auto accepted = ConnectBlock(just_in_time, coins, Params());
    EXPECT_TRUE(accepted.has_value()) << consensus::Describe(accepted.error());
}

TEST(ConnectBlock, AProvablyUnspendableOutputIsNeverStored) {
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    Transaction coinbase = MakeCoinbase(FUNDING_HEIGHT, {1'000, 0});
    coinbase.outputs[1].lock = Lock{};
    ASSERT_TRUE(coinbase.outputs[1].lock.IsUnspendable());
    const Block block = MakeBlock(FUNDING_HEIGHT, {coinbase});

    const auto result = ConnectBlock(block, coins, Params());
    ASSERT_TRUE(result.has_value()) << consensus::Describe(result.error());
    const Hash256 txid = block.transactions.front().Txid();
    EXPECT_TRUE(coins.HaveCoin(OutPoint{.txid = txid, .index = 0}));
    EXPECT_FALSE(coins.HaveCoin(OutPoint{.txid = txid, .index = 1}));
    EXPECT_EQ(coins.ChangeCount(), 1U);

    // Disconnect must skip exactly the same output, or it would look for a coin that was
    // never stored.
    const auto disconnected = DisconnectBlock(block, result->undo, coins);
    ASSERT_TRUE(disconnected.has_value()) << Describe(disconnected.error());
    EXPECT_EQ(coins.ChangeCount(), 0U);
}

TEST(ConnectBlock, ACoinbaseMayClaimTheRewardPlusTheFeesTheBlockActuallyPaid) {
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const OutPoint funded = FirstOutputOf(Fund(coins, {1'000}));
    const size_t changes_before = coins.ChangeCount();

    const int64_t reward = BlockReward(MATURE_HEIGHT, Params().issuance);
    constexpr int64_t FEE = 400;

    const Block too_much = MakeBlock(
        MATURE_HEIGHT,
        {MakeCoinbase(MATURE_HEIGHT, {reward + FEE + 1}), MakeSpend(funded, 1'000 - FEE)});
    const auto rejected = ConnectBlock(too_much, coins, Params());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::BlockCoinbasePaysTooMuch);
    EXPECT_EQ(coins.ChangeCount(), changes_before);

    const Block exactly =
        MakeBlock(MATURE_HEIGHT,
                  {MakeCoinbase(MATURE_HEIGHT, {reward + FEE}), MakeSpend(funded, 1'000 - FEE)});
    const auto accepted = ConnectBlock(exactly, coins, Params());
    ASSERT_TRUE(accepted.has_value()) << consensus::Describe(accepted.error());
    EXPECT_EQ(accepted->total_fees, FEE);
}

TEST(ConnectBlock, ATransactionMaySpendAnOutputCreatedEarlierInTheSameBlockButNotLater) {
    const EmptyCoinsView empty;
    CoinsCache funded_set = CoinsCache::Over(empty);
    const OutPoint funded = FirstOutputOf(Fund(funded_set, {1'000}));

    const Transaction first = MakeSpend(funded, 900);
    const Transaction second = MakeSpend(OutPoint{.txid = first.Txid(), .index = 0}, 800);
    const Transaction coinbase = MakeCoinbase(MATURE_HEIGHT, {500});

    // Both attempts start from the same state, each in its own layer over it.
    CoinsCache out_of_order = CoinsCache::Over(funded_set);
    const auto rejected =
        ConnectBlock(MakeBlock(MATURE_HEIGHT, {coinbase, second, first}), out_of_order, Params());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::TxInputMissingOrSpent);

    CoinsCache in_order = CoinsCache::Over(funded_set);
    const auto accepted =
        ConnectBlock(MakeBlock(MATURE_HEIGHT, {coinbase, first, second}), in_order, Params());
    ASSERT_TRUE(accepted.has_value()) << consensus::Describe(accepted.error());
    EXPECT_EQ(accepted->total_fees, 200);
}

TEST(ConnectBlock, CreatingAnOutpointThatIsAlreadyUnspentIsRejected) {
    // Unreachable on a real chain — a coinbase txid carries its height, so no two coinbases
    // share one — but the set enforces the invariant rather than trusting it, because the
    // cost of being wrong is a live coin silently overwritten.
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const Block block = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, {1'000})});
    ASSERT_TRUE(ConnectBlock(block, coins, Params()).has_value());

    const auto again = ConnectBlock(block, coins, Params());
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), ValidationError::TxCreatesExistingOutpoint);
}

TEST(DisconnectBlock, AStoredCoinIsComparedInFullRatherThanForPresenceAlone) {
    // Same transactions, a different header height. The txid is unchanged, so the outpoint
    // is found — but the coin there was created at another height, which is what a caller
    // disconnecting the wrong block looks like. Presence alone would accept it and leave
    // two nodes holding different sets while both believed they agreed.
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const Block block = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, {1'000})});
    const auto connected = ConnectBlock(block, coins, Params());
    ASSERT_TRUE(connected.has_value()) << consensus::Describe(connected.error());

    Block wrong_height = block;
    wrong_height.header.height = FUNDING_HEIGHT + 1;
    const auto result = DisconnectBlock(wrong_height, connected->undo, coins);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisconnectError::CreatedCoinDoesNotMatch);
    // Refused, and the set is still the one the real block produced.
    EXPECT_TRUE(coins.HaveCoin(FirstOutputOf(block)));
}

TEST(DisconnectBlock, AnUndoRecordThatDoesNotDescribeTheBlockIsRefused) {
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const OutPoint funded = FirstOutputOf(Fund(coins, {1'000}));
    const Block spending =
        MakeBlock(MATURE_HEIGHT, {MakeCoinbase(MATURE_HEIGHT, {500}), MakeSpend(funded, 600)});
    ASSERT_TRUE(ConnectBlock(spending, coins, Params()).has_value());

    const auto result = DisconnectBlock(spending, BlockUndo{}, coins);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DisconnectError::UndoDoesNotDescribeBlock);
}

TEST(BlockUndo, RoundTripsThroughItsEncoding) {
    // A reorganisation may begin after a restart, so an undo record has to survive one.
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const OutPoint funded = FirstOutputOf(Fund(coins, {1'000}));
    const Block spending =
        MakeBlock(MATURE_HEIGHT, {MakeCoinbase(MATURE_HEIGHT, {500}), MakeSpend(funded, 600)});
    const auto connected = ConnectBlock(spending, coins, Params());
    ASSERT_TRUE(connected.has_value()) << consensus::Describe(connected.error());

    Writer writer;
    connected->undo.Serialize(writer);
    Reader reader(writer.Bytes());
    BlockUndo decoded;
    ASSERT_TRUE(BlockUndo::Deserialize(reader, decoded, Params()));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded, connected->undo);
}

}  // namespace
}  // namespace amarian::utxo
