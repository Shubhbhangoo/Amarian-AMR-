/// \file
/// Phase 2's acceptance criterion: invalid inflation attempts are rejected.
///
/// Every test here is an attempt to create money, and every one must fail. That is the whole
/// difference between a hard cap that is enforced and a hard cap that is documented — the
/// schedule in `issuance.hpp` says how many facets a height may create, and this file is the
/// evidence that a block claiming more does not get in.
///
/// These live in the `consensus` tier rather than in `tests/unit/` because "did I break the
/// supply guarantee" is a different question from "did I break the build", and it should be
/// answerable by `ctest -L consensus` without waiting for anything else.
///
/// Coins here are locked with an unknown lock version, which consensus deliberately treats as
/// spendable by any witness so a new lock form can be soft-forked in. That keeps these tests
/// on the real `ConnectBlock` path with no signing dependency: what is under test is the
/// arithmetic, and a signature would only make each case slower to build without making any
/// of them sharper. Spend authorisation has its own tests against real ML-DSA signatures.

#include <amarian/consensus/issuance.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/utxo/connect.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace amarian::utxo {
namespace {

using consensus::ValidationError;

/// A lock version consensus does not know, and therefore treats as spendable by anything.
constexpr uint8_t FUTURE_LOCK_VERSION = 200;

/// Regtest, for its 20-block coinbase maturity: short enough to build the gap in one line.
[[nodiscard]] const ChainParams& Params() noexcept {
    return REGTEST_PARAMS;
}

constexpr uint32_t FUNDING_HEIGHT = 1;
constexpr uint32_t SPEND_HEIGHT = FUNDING_HEIGHT + REGTEST_PARAMS.coinbase_maturity;

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

/// A transaction spending every outpoint given and paying `paid` to one output.
///
/// Multi-input because two of the attacks below are about what happens when the same outpoint
/// appears twice — once within a single transaction, once across two.
[[nodiscard]] Transaction MakeSpend(const std::vector<OutPoint>& outpoints, int64_t paid) {
    Transaction tx;
    tx.version = 1;
    for (const OutPoint& outpoint : outpoints) {
        tx.inputs.push_back(TxInput{.outpoint = outpoint, .sequence = 0});
        tx.witnesses.push_back(Witness{});
    }
    tx.outputs = {TxOutput{.amount = paid, .lock = SpendableLock()}};
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

/// A connected block whose coinbase pays `amounts` at `FUNDING_HEIGHT`.
[[nodiscard]] Block Fund(CoinsCache& coins, const std::vector<int64_t>& amounts) {
    const Block funding = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, amounts)});
    const auto connected = ConnectBlock(funding, coins, Params());
    if (!connected.has_value()) {
        ADD_FAILURE() << consensus::Describe(connected.error());
    }
    return funding;
}

TEST(Inflation, ACoinbaseCannotClaimFeesNoTransactionPaid) {
    // The phantom fee. A block whose only transaction is its coinbase collected nothing, so
    // the ceiling is the scheduled reward exactly. One facet above it is minting, and the
    // node has no running supply total it could be talked out of — the bound comes from the
    // height.
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const int64_t reward = BlockReward(FUNDING_HEIGHT, Params().issuance);
    ASSERT_GT(reward, 0);

    const Block too_much = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, {reward + 1})});
    const auto rejected = ConnectBlock(too_much, coins, Params());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::BlockCoinbasePaysTooMuch);
    EXPECT_EQ(coins.ChangeCount(), 0U);

    // Split across two outputs so the rule is on the sum rather than on any one output.
    const Block split = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, {reward, 1})});
    const auto also_rejected = ConnectBlock(split, coins, Params());
    ASSERT_FALSE(also_rejected.has_value());
    EXPECT_EQ(also_rejected.error(), ValidationError::BlockCoinbasePaysTooMuch);
    EXPECT_EQ(coins.ChangeCount(), 0U);

    const Block exactly = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, {reward})});
    const auto accepted = ConnectBlock(exactly, coins, Params());
    ASSERT_TRUE(accepted.has_value()) << consensus::Describe(accepted.error());
    EXPECT_EQ(accepted->total_fees, 0);
}

TEST(Inflation, TheSameOutpointCannotBeSpentTwiceInOneBlock) {
    const EmptyCoinsView empty;
    CoinsCache funded_set = CoinsCache::Over(empty);
    const OutPoint funded = FirstOutputOf(Fund(funded_set, {1'000}));
    const size_t changes_before = funded_set.ChangeCount();

    // Twice across two transactions. The first spend removes the coin, so the second finds
    // nothing — the double spend is impossible rather than detected, which is the stronger
    // property: there is no comparison that could be got wrong.
    CoinsCache two_transactions = CoinsCache::Over(funded_set);
    const Block across = MakeBlock(
        SPEND_HEIGHT,
        {MakeCoinbase(SPEND_HEIGHT, {0}), MakeSpend({funded}, 400), MakeSpend({funded}, 400)});
    const auto rejected_across = ConnectBlock(across, two_transactions, Params());
    ASSERT_FALSE(rejected_across.has_value());
    EXPECT_EQ(rejected_across.error(), ValidationError::TxInputMissingOrSpent);

    // Twice within one transaction, which is a different code path: the same loop, spending
    // the same outpoint on consecutive iterations.
    CoinsCache one_transaction = CoinsCache::Over(funded_set);
    const Block within = MakeBlock(
        SPEND_HEIGHT, {MakeCoinbase(SPEND_HEIGHT, {0}), MakeSpend({funded, funded}, 1'900)});
    const auto rejected_within = ConnectBlock(within, one_transaction, Params());
    ASSERT_FALSE(rejected_within.has_value());
    EXPECT_EQ(rejected_within.error(), ValidationError::TxInputMissingOrSpent);

    // Neither attempt cost the set anything, and the coin is still there to be spent once.
    EXPECT_EQ(funded_set.ChangeCount(), changes_before);
    EXPECT_TRUE(funded_set.HaveCoin(funded));
}

TEST(Inflation, ACoinSpentByAnEarlierBlockCannotBeSpentAgain) {
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const OutPoint funded = FirstOutputOf(Fund(coins, {1'000}));

    const Transaction spend = MakeSpend({funded}, 600);
    const Block first = MakeBlock(SPEND_HEIGHT, {MakeCoinbase(SPEND_HEIGHT, {400}), spend});
    const auto connected = ConnectBlock(first, coins, Params());
    ASSERT_TRUE(connected.has_value()) << consensus::Describe(connected.error());
    ASSERT_FALSE(coins.HaveCoin(funded));

    // A second block spending the same coin. Nothing about it is malformed — it is the same
    // transaction that was valid one block ago — and it fails only because the coin it names
    // is no longer in the set. That is what makes the UTXO set the record of what exists.
    const Block second =
        MakeBlock(SPEND_HEIGHT + 1, {MakeCoinbase(SPEND_HEIGHT + 1, {400}), spend});
    const auto rejected = ConnectBlock(second, coins, Params());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::TxInputMissingOrSpent);
}

/// How many outputs of `MAX_MONEY` it takes for the untruncated sum to wrap past 2^64 and land
/// back inside `[0, MAX_MONEY]`.
constexpr uint32_t WRAPPING_COUNT = 220;

// The premise of the two tests below, proved at compile time rather than asserted in prose.
// Unsigned arithmetic wraps by definition, so this is exactly the value a signed accumulator
// would hold under `-fwrapv` after the same additions.
constexpr uint64_t WRAPPED_SUM = uint64_t{WRAPPING_COUNT} * static_cast<uint64_t>(MAX_MONEY);
static_assert(WRAPPED_SUM < static_cast<uint64_t>(MAX_MONEY),
              "the engineered sum must land back inside the money range, or these tests are "
              "asserting nothing an ordinary range check would not already catch");
static_assert(WRAPPED_SUM > 0, "a wrapped total of zero would be rejected for a different reason");

TEST(Inflation, ACoinbaseOutputSumEngineeredToWrapIsRejectedByTheAccumulation) {
    // Every output is individually a valid amount, and the sum of them is *also* a valid
    // amount once it has wrapped — so a rule that summed first and range-checked afterwards
    // would see nothing wrong with 220 times the entire money supply.
    //
    // The sum is guarded twice: by the accumulation, and by the ceiling that follows it. This
    // asserts the first one fires, because the second only catches this case if the wrapped
    // value happens to exceed the permitted reward, and "happens to" is not a consensus rule.
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const std::vector<int64_t> amounts(WRAPPING_COUNT, MAX_MONEY);
    const Block block = MakeBlock(FUNDING_HEIGHT, {MakeCoinbase(FUNDING_HEIGHT, amounts)});

    const auto rejected = ConnectBlock(block, coins, Params());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::TxOutputSumOutOfRange);
    EXPECT_EQ(coins.ChangeCount(), 0U);
}

TEST(Inflation, ABlockFeeTotalEngineeredToWrapIsRejectedByTheAccumulation) {
    // The same wrap, one layer up: fees are summed across a block, and the total is what the
    // coinbase is allowed to add to its reward. A wrapped total back inside the money range
    // would be handed to `CheckCoinbaseAmount` as a legitimate fee figure.
    //
    // The set this starts from holds 220 times the money that will ever exist, which no chain
    // can reach. That is the point: the fee arithmetic must not be the reason it cannot, because
    // then a corrupted database would be a minting bug rather than a corrupted database.
    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);

    std::vector<Transaction> transactions = {MakeCoinbase(SPEND_HEIGHT, {0})};
    for (uint32_t index = 0; index < WRAPPING_COUNT; ++index) {
        // Distinct by index under a zero txid, which cannot collide with the coinbase sentinel
        // — that is the zero txid at index 0xFFFFFFFF specifically.
        const OutPoint outpoint{.txid = Hash256{}, .index = index};
        const Coin coin{.output = TxOutput{.amount = MAX_MONEY, .lock = SpendableLock()},
                        .height = FUNDING_HEIGHT,
                        .is_coinbase = false};
        ASSERT_TRUE(coins.AddCoin(outpoint, coin)) << "seeding outpoint " << index;
        // Pays nothing out, so the whole of a spent coin is fee.
        transactions.push_back(MakeSpend({outpoint}, 0));
    }
    const size_t seeded = coins.ChangeCount();

    const auto rejected = ConnectBlock(MakeBlock(SPEND_HEIGHT, transactions), coins, Params());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::BlockFeesOutOfRange);
    // The block was refused whole: nothing it spent left the set and nothing it created entered.
    EXPECT_EQ(coins.ChangeCount(), seeded);
}

TEST(Inflation, OnceIssuanceEndsACoinbaseMayClaimFeesAndNothingElse) {
    // Mainnet's schedule, at the first height it pays nothing. There is no tail emission, so
    // from here the only coins a coinbase may claim are ones somebody else already owned.
    const ChainParams& mainnet = MAINNET_PARAMS;
    const uint64_t end = IssuanceEndHeight(mainnet.issuance);
    ASSERT_LE(end, uint64_t{UINT32_MAX});
    const uint32_t height = static_cast<uint32_t>(end);
    ASSERT_EQ(BlockReward(height, mainnet.issuance), 0);
    // And the block before it still paid, so this is the boundary rather than a height chosen
    // from somewhere in the long zero tail.
    ASSERT_GT(BlockReward(height - 1, mainnet.issuance), 0);

    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);

    const Block one_facet = MakeBlock(height, {MakeCoinbase(height, {1})});
    const auto rejected = ConnectBlock(one_facet, coins, mainnet);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::BlockCoinbasePaysTooMuch);
    EXPECT_EQ(coins.ChangeCount(), 0U);

    // Claiming nothing is still a valid block: the chain continues, it just stops paying.
    const Block nothing = MakeBlock(height, {MakeCoinbase(height, {0})});
    const auto accepted = ConnectBlock(nothing, coins, mainnet);
    ASSERT_TRUE(accepted.has_value()) << consensus::Describe(accepted.error());
    EXPECT_EQ(accepted->total_fees, 0);
}

TEST(Inflation, AFeeAfterIssuanceEndsRaisesTheCeilingByExactlyThatFee) {
    // The other half of the rule above. "Fees and nothing else" is not "nothing" — a miner is
    // still paid, just only out of what already exists. Era 178 pays one facet per block, which
    // makes the whole arithmetic here single-facet and the boundary impossible to miss.
    const ChainParams& mainnet = MAINNET_PARAMS;
    const uint32_t height = static_cast<uint32_t>(IssuanceEndHeight(mainnet.issuance));
    const uint32_t funding_height = height - mainnet.coinbase_maturity;
    const int64_t funded_amount = BlockReward(funding_height, mainnet.issuance);
    ASSERT_EQ(funded_amount, 1);

    const EmptyCoinsView empty;
    CoinsCache coins = CoinsCache::Over(empty);
    const Block funding =
        MakeBlock(funding_height, {MakeCoinbase(funding_height, {funded_amount})});
    ASSERT_TRUE(ConnectBlock(funding, coins, mainnet).has_value());
    const OutPoint funded = FirstOutputOf(funding);

    // Paying nothing out makes the whole coin a fee, so the block's ceiling is exactly one facet.
    const Transaction spend = MakeSpend({funded}, 0);
    CoinsCache too_much = CoinsCache::Over(coins);
    const auto rejected = ConnectBlock(
        MakeBlock(height, {MakeCoinbase(height, {funded_amount + 1}), spend}), too_much, mainnet);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), ValidationError::BlockCoinbasePaysTooMuch);

    CoinsCache exactly = CoinsCache::Over(coins);
    const auto accepted = ConnectBlock(
        MakeBlock(height, {MakeCoinbase(height, {funded_amount}), spend}), exactly, mainnet);
    ASSERT_TRUE(accepted.has_value()) << consensus::Describe(accepted.error());
    EXPECT_EQ(accepted->total_fees, funded_amount);
}

}  // namespace
}  // namespace amarian::utxo
