#pragma once

/// \file
/// The issuance schedule, as a consensus rule.
///
/// Every node computes a block's scheduled reward from its height and nothing
/// else. There is no running total to trust, no state that could drift between
/// nodes, no operator setting, and no authority that can mint. A block whose
/// coinbase claims one facet more than this function returns for its height is
/// invalid, and that is the entirety of the supply guarantee.
///
/// The schedule is defined in ECONOMICS.md and reproduced here in the only form
/// that matters — executable. Its parameters are per-network because the test
/// networks use the same shape with shorter eras; the mainnet parameters are
/// settled, and changing any of them changes the monetary system this project
/// exists to keep fixed.
///
/// The functions are `constexpr` for a specific reason: it makes the realised
/// supply cap a compile-time fact rather than a documented claim. The
/// `static_assert`s at the bottom of this file fail the build if the schedule
/// stops summing to `MAX_MONEY`, so the cap cannot be quietly changed by an edit
/// to the decay, the era length, or the initial reward.

#include <amarian/primitives/amount.hpp>

#include <cstdint>

namespace amarian {

/// Facets in one AMR: 10^10, ten decimal places.
///
/// Consensus arithmetic is in facets only. Nothing below the RPC boundary knows
/// this constant exists, and no consensus value is ever expressed in AMR.
inline constexpr int64_t FACETS_PER_AMR = 10'000'000'000LL;

/// Per-era reward retention: `reward = reward * 7 / 8`, integer division.
///
/// A 12.5% cut roughly annually rather than a 50% cut in a single block. The
/// gradient is 5.7x gentler at each step while decaying slightly *faster* overall
/// than a four-year halving; the reasoning is in ECONOMICS.md.
inline constexpr int64_t REWARD_RETENTION_NUMERATOR = 7;
inline constexpr int64_t REWARD_RETENTION_DENOMINATOR = 8;

/// The three numbers that define a network's issuance.
struct IssuanceParams {
    /// Blocks per era. Zero is not a valid schedule and yields no issuance at all,
    /// which is the safe direction for a misconfiguration to fail in.
    uint32_t era_blocks;
    int64_t initial_reward;
};

/// Mainnet: 105 000-block eras, 10 AMR to start. Settled.
inline constexpr IssuanceParams MAINNET_ISSUANCE{
    .era_blocks = 105'000,
    .initial_reward = 10 * FACETS_PER_AMR,
};

/// Which era a height falls in. Era 0 begins at the genesis block.
[[nodiscard]] constexpr uint32_t EraOfHeight(uint32_t height,
                                             const IssuanceParams& params) noexcept {
    if (params.era_blocks == 0) {
        return 0;
    }
    return height / params.era_blocks;
}

/// The scheduled reward in facets for a block at `height`.
///
/// A pure function of the height: divide by the era length, apply the retention
/// that many times, done. No special case anywhere, and no undefined behaviour at
/// any height — repeated `* 7 / 8` reaches zero and stays there, unlike a shift by
/// an era count that eventually exceeds the width of the type.
[[nodiscard]] constexpr int64_t BlockReward(uint32_t height,
                                            const IssuanceParams& params) noexcept {
    const uint32_t era = EraOfHeight(height, params);
    if (params.era_blocks == 0) {
        return 0;
    }
    int64_t reward = params.initial_reward;
    for (uint32_t index = 0; index < era; ++index) {
        // Once the reward is zero it stays zero: there is no tail emission, so
        // this both terminates the loop early and is the rule itself.
        if (reward <= 0) {
            return 0;
        }
        // reward * 7 is at most 7 * 10^11 for any real parameters, so the
        // intermediate cannot overflow int64_t.
        reward = reward * REWARD_RETENTION_NUMERATOR / REWARD_RETENTION_DENOMINATOR;
    }
    return reward > 0 ? reward : 0;
}

/// Number of eras that pay a nonzero reward.
[[nodiscard]] constexpr uint32_t EraCountWithReward(const IssuanceParams& params) noexcept {
    if (params.era_blocks == 0) {
        return 0;
    }
    uint32_t eras = 0;
    int64_t reward = params.initial_reward;
    while (reward > 0) {
        ++eras;
        reward = reward * REWARD_RETENTION_NUMERATOR / REWARD_RETENTION_DENOMINATOR;
    }
    return eras;
}

/// The first height whose scheduled reward is zero. From here a coinbase may claim
/// transaction fees and nothing else.
[[nodiscard]] constexpr uint64_t IssuanceEndHeight(const IssuanceParams& params) noexcept {
    return static_cast<uint64_t>(EraCountWithReward(params)) * params.era_blocks;
}

/// Total facets the schedule emits if every block is mined: the realised cap.
///
/// This is the *realised* figure, not the closed form. Integer division truncates
/// the reward at every era boundary and the shortfalls accumulate, so the ideal
/// 8 400 000 AMR is a strict upper bound that no execution can reach. The number
/// this returns is the one the software actually enforces, and it is therefore the
/// one that belongs in every claim about supply.
[[nodiscard]] constexpr int64_t TotalIssuance(const IssuanceParams& params) noexcept {
    int64_t total = 0;
    int64_t reward = params.initial_reward;
    while (reward > 0) {
        total += reward * params.era_blocks;
        reward = reward * REWARD_RETENTION_NUMERATOR / REWARD_RETENTION_DENOMINATOR;
    }
    return total;
}

// --- The supply cap, verified at compile time -------------------------------
//
// These are not tests; they are the build refusing to produce a node whose
// monetary policy differs from the specified one. An edit to the decay, the era
// length, or the initial reward that changes the cap cannot compile.

static_assert(TotalIssuance(MAINNET_ISSUANCE) == MAX_MONEY,
              "the mainnet schedule must sum to exactly MAX_MONEY");
static_assert(EraCountWithReward(MAINNET_ISSUANCE) == 179,
              "179 eras pay a reward: era 0 through era 178");
static_assert(IssuanceEndHeight(MAINNET_ISSUANCE) == 18'795'000,
              "issuance ends at height 18 795 000");
static_assert(BlockReward(18'795'000, MAINNET_ISSUANCE) == 0, "there is no tail emission");
static_assert(BlockReward(18'794'999, MAINNET_ISSUANCE) == 1,
              "era 178 pays exactly one facet per block");

// The published schedule table, era by era, as compile-time vectors.
static_assert(BlockReward(0, MAINNET_ISSUANCE) == 100'000'000'000);
static_assert(BlockReward(104'999, MAINNET_ISSUANCE) == 100'000'000'000);
static_assert(BlockReward(105'000, MAINNET_ISSUANCE) == 87'500'000'000);
static_assert(BlockReward(210'000, MAINNET_ISSUANCE) == 76'562'500'000);
static_assert(BlockReward(315'000, MAINNET_ISSUANCE) == 66'992'187'500);
static_assert(BlockReward(420'000, MAINNET_ISSUANCE) == 58'618'164'062);
static_assert(BlockReward(525'000, MAINNET_ISSUANCE) == 51'290'893'554);
static_assert(BlockReward(630'000, MAINNET_ISSUANCE) == 44'879'531'859);
static_assert(BlockReward(735'000, MAINNET_ISSUANCE) == 39'269'590'376);
static_assert(BlockReward(840'000, MAINNET_ISSUANCE) == 34'360'891'579);
static_assert(BlockReward(945'000, MAINNET_ISSUANCE) == 30'065'780'131);
static_assert(BlockReward(1'050'000, MAINNET_ISSUANCE) == 26'307'557'614);
static_assert(BlockReward(1'155'000, MAINNET_ISSUANCE) == 23'019'112'912);

// A malformed schedule emits nothing rather than dividing by zero.
static_assert(BlockReward(0, IssuanceParams{.era_blocks = 0, .initial_reward = 1}) == 0);
static_assert(TotalIssuance(IssuanceParams{.era_blocks = 105'000, .initial_reward = 0}) == 0);

}  // namespace amarian
