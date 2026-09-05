#include <amarian/consensus/issuance.hpp>
#include <amarian/primitives/amount.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace amarian {
namespace {

/// The per-height rule and the per-era summation are two separate pieces of code,
/// and the supply cap rests on the second while block validation will use the
/// first. If they ever disagree, the enforced cap is not the claimed cap.
TEST(Issuance, PerHeightRewardsSumToTheRealisedCap) {
    int64_t total = 0;
    const uint32_t eras = EraCountWithReward(MAINNET_ISSUANCE);
    for (uint32_t era = 0; era < eras; ++era) {
        const uint32_t height = era * MAINNET_ISSUANCE.era_blocks;
        const int64_t reward = BlockReward(height, MAINNET_ISSUANCE);
        // Every block within an era pays the same reward, so checking both ends of
        // the era is enough to multiply by its length.
        EXPECT_EQ(BlockReward(height + MAINNET_ISSUANCE.era_blocks - 1, MAINNET_ISSUANCE), reward)
            << "era " << era;
        total += reward * MAINNET_ISSUANCE.era_blocks;
    }
    EXPECT_EQ(total, MAX_MONEY);
    EXPECT_EQ(total, TotalIssuance(MAINNET_ISSUANCE));
}

TEST(Issuance, TheRewardNeverRises) {
    // Sampled at every era boundary and either side of it, which is where a
    // monotonicity break could actually hide.
    int64_t previous = BlockReward(0, MAINNET_ISSUANCE);
    for (uint32_t era = 1; era <= EraCountWithReward(MAINNET_ISSUANCE) + 1; ++era) {
        const uint32_t boundary = era * MAINNET_ISSUANCE.era_blocks;
        EXPECT_LE(BlockReward(boundary - 1, MAINNET_ISSUANCE), previous) << "era " << era;
        const int64_t current = BlockReward(boundary, MAINNET_ISSUANCE);
        EXPECT_LE(current, previous) << "era " << era;
        previous = current;
    }
}

TEST(Issuance, EmissionStopsAndStaysStopped) {
    const uint64_t end = IssuanceEndHeight(MAINNET_ISSUANCE);
    ASSERT_GT(BlockReward(static_cast<uint32_t>(end) - 1, MAINNET_ISSUANCE), 0);
    EXPECT_EQ(BlockReward(static_cast<uint32_t>(end), MAINNET_ISSUANCE), 0);

    // No tail emission at any later height, including the end of the type's range.
    for (const uint32_t height : {static_cast<uint32_t>(end) + 1,
                                  static_cast<uint32_t>(end) * 2,
                                  100'000'000U,
                                  UINT32_MAX}) {
        EXPECT_EQ(BlockReward(height, MAINNET_ISSUANCE), 0) << "height " << height;
    }
}

TEST(Issuance, EveryRewardIsASpendableAmount) {
    // A reward outside [0, MAX_MONEY] would be rejected by the amount range check
    // at deserialisation, making the schedule unminable rather than merely wrong.
    for (uint32_t era = 0; era <= EraCountWithReward(MAINNET_ISSUANCE); ++era) {
        const int64_t reward = BlockReward(era * MAINNET_ISSUANCE.era_blocks, MAINNET_ISSUANCE);
        EXPECT_TRUE(IsValidAmount(reward)) << "era " << era;
    }
}

TEST(Issuance, ShorterErasEmitProportionallyLess) {
    // The shape the test networks use: same decay and same initial reward, shorter
    // eras. The cap must scale with the era length and nothing else.
    constexpr IssuanceParams short_eras{.era_blocks = 1'050,
                                        .initial_reward = MAINNET_ISSUANCE.initial_reward};
    EXPECT_EQ(EraCountWithReward(short_eras), EraCountWithReward(MAINNET_ISSUANCE));
    EXPECT_EQ(TotalIssuance(short_eras) * 100, TotalIssuance(MAINNET_ISSUANCE));
}

TEST(Issuance, AZeroEraLengthEmitsNothing) {
    // A misconfigured network must fail closed, not divide by zero.
    constexpr IssuanceParams broken{.era_blocks = 0, .initial_reward = 100'000'000'000};
    EXPECT_EQ(EraOfHeight(12'345, broken), 0U);
    EXPECT_EQ(BlockReward(12'345, broken), 0);
    EXPECT_EQ(EraCountWithReward(broken), 0U);
    EXPECT_EQ(IssuanceEndHeight(broken), 0U);
}

}  // namespace
}  // namespace amarian
