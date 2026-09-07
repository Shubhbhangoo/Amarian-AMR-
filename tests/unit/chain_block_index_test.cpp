/// \file
/// Tests for the tree of known headers and the rule that picks a tip out of it.
///
/// Selection is not a property of any one block, so these tests are about sequences: a
/// branch arriving after a rival, a rival being ruled out, a tip moving. Regtest is used
/// throughout because its target floor makes a header cost a couple of hash attempts, so
/// a test can mine real proof of work rather than bypass the check.

#include <amarian/chain/block_index.hpp>

#include <amarian/consensus/asert.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <variant>
#include <vector>

namespace amarian::chain {
namespace {

[[nodiscard]] const ChainParams& Params() noexcept {
    return REGTEST_PARAMS;
}

/// Far enough ahead of every timestamp these tests use that the future-block rule never
/// fires. Whether that rule is right is `consensus`'s question, not this file's.
constexpr int64_t NOW = 4'000'000'000;

/// A header extending `parent`, mined against the floor it inherits.
///
/// `tag` fills the merkle root, which is what makes two siblings different headers with
/// different hashes; nothing here checks a merkle root, since that is `CheckBlock`'s job
/// and the index holds no bodies. Callers must give siblings distinct tags.
[[nodiscard]] BlockHeader ChildOf(const BlockIndexEntry& parent, uint8_t tag) {
    BlockHeader header;
    header.version = parent.header.version;
    header.height = parent.height + 1;
    header.prev_block = parent.hash;
    header.merkle_root.Array().fill(tag);
    header.timestamp = parent.header.timestamp + Params().target_block_seconds;
    header.target_bits = parent.header.target_bits;
    header.nonce = 0;
    while (!CheckProofOfWork(header.Hash(), header.target_bits)) {
        ++header.nonce;
    }
    return header;
}

/// Mines a child of `parent` and adds it, failing the test if the index refuses it.
[[nodiscard]] const BlockIndexEntry*
Extend(BlockIndex& index, const BlockIndexEntry& parent, uint8_t tag) {
    const std::expected<const BlockIndexEntry*, HeaderError> added =
        index.AddHeader(ChildOf(parent, tag), NOW, Params());
    if (!added.has_value()) {
        ADD_FAILURE() << "header rejected: " << Describe(added.error());
        return nullptr;
    }
    return *added;
}

// --- The index's starting state ---------------------------------------------

TEST(BlockIndex, StartsAtTheNetworksGenesisAndNothingElse) {
    const BlockIndex index = BlockIndex::ForNetwork(Params());

    EXPECT_EQ(index.Size(), 1U);
    EXPECT_EQ(index.Genesis().hash, Params().genesis_hash);
    EXPECT_EQ(index.Genesis().height, 0U);
    EXPECT_EQ(index.Genesis().parent, nullptr);
    EXPECT_EQ(index.Genesis().total_work, Work::OfCompactTarget(Params().genesis_bits));
    EXPECT_EQ(index.BestHeader(), &index.Genesis());

    // Full rather than Header: genesis is a chain parameter, and there is no weaker state
    // for a block whose only output can never be spent to sit in.
    EXPECT_EQ(index.Genesis().validity, BlockValidity::Full);
}

// --- Accepting and refusing headers -----------------------------------------

TEST(BlockIndex, AHeaderExtendingTheChainBecomesTheBestTip) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* child = Extend(index, index.Genesis(), 1);
    ASSERT_NE(child, nullptr);

    EXPECT_EQ(index.Size(), 2U);
    EXPECT_EQ(child->height, 1U);
    EXPECT_EQ(child->parent, &index.Genesis());
    EXPECT_EQ(child->total_work,
              index.Genesis().total_work + Work::OfCompactTarget(child->header.target_bits));
    EXPECT_EQ(index.BestHeader(), child);
    EXPECT_EQ(child->validity, BlockValidity::Header);
    EXPECT_TRUE(child->IsEligible());
}

TEST(BlockIndex, AHeaderWithAnUnknownPredecessorIsRefusedAndNotRemembered) {
    BlockIndex index = BlockIndex::ForNetwork(Params());

    // A header at height 1 that is never offered, and its child, which therefore has a
    // predecessor the index has not heard of. Headers arriving out of order is routine.
    BlockIndexEntry unoffered;
    unoffered.header = ChildOf(index.Genesis(), 1);
    unoffered.hash = unoffered.header.Hash();
    unoffered.height = 1;

    const BlockHeader grandchild = ChildOf(unoffered, 2);
    const std::expected<const BlockIndexEntry*, HeaderError> early =
        index.AddHeader(grandchild, NOW, Params());
    ASSERT_FALSE(early.has_value());
    ASSERT_TRUE(std::holds_alternative<IndexError>(early.error()));
    EXPECT_EQ(std::get<IndexError>(early.error()), IndexError::UnknownPredecessor);
    EXPECT_EQ(index.Size(), 1U);

    // Offering the predecessor and then the same header again succeeds, which is the half
    // that matters: a refusal for being early must not be recorded as a verdict.
    ASSERT_NE(Extend(index, index.Genesis(), 1), nullptr);
    const std::expected<const BlockIndexEntry*, HeaderError> later =
        index.AddHeader(grandchild, NOW, Params());
    ASSERT_TRUE(later.has_value()) << Describe(later.error());
    EXPECT_EQ((*later)->height, 2U);
}

TEST(BlockIndex, AHeaderFailingItsOwnRulesIsRefusedWithTheRulesVerdict) {
    BlockIndex index = BlockIndex::ForNetwork(Params());

    // Real proof of work, then the nonce changed: the header no longer meets its target.
    BlockHeader forged = ChildOf(index.Genesis(), 1);
    forged.nonce += 1;
    while (CheckProofOfWork(forged.Hash(), forged.target_bits)) {
        forged.nonce += 1;
    }

    const std::expected<const BlockIndexEntry*, HeaderError> refused =
        index.AddHeader(forged, NOW, Params());
    ASSERT_FALSE(refused.has_value());
    // A rule violation, not an index-level reason: the two call for opposite reactions
    // towards whoever sent it, which is why they are separate halves of the sum type.
    EXPECT_TRUE(std::holds_alternative<consensus::ValidationError>(refused.error()));
    EXPECT_EQ(index.Size(), 1U);
}

TEST(BlockIndex, OfferingTheSameHeaderTwiceReturnsTheSameEntry) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockHeader header = ChildOf(index.Genesis(), 1);

    const std::expected<const BlockIndexEntry*, HeaderError> first =
        index.AddHeader(header, NOW, Params());
    ASSERT_TRUE(first.has_value()) << Describe(first.error());
    const std::expected<const BlockIndexEntry*, HeaderError> second =
        index.AddHeader(header, NOW, Params());
    ASSERT_TRUE(second.has_value()) << Describe(second.error());

    EXPECT_EQ(*first, *second);
    EXPECT_EQ(index.Size(), 2U);
}

// --- Choosing between branches ----------------------------------------------

TEST(BlockIndex, EqualWorkKeepsTheBranchSeenFirst) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* first = Extend(index, index.Genesis(), 1);
    ASSERT_NE(first, nullptr);
    const BlockIndexEntry* second = Extend(index, index.Genesis(), 2);
    ASSERT_NE(second, nullptr);

    // Identical work, so the tie-break decides — and it decides for the one that arrived
    // first, which is what makes withholding a block lose rather than cost nothing.
    ASSERT_EQ(first->total_work, second->total_work);
    EXPECT_EQ(index.BestHeader(), first);
    EXPECT_TRUE(IsBetterTip(*first, *second));
    EXPECT_FALSE(IsBetterTip(*second, *first));

    // And it is a strict order: nothing is better than itself.
    EXPECT_FALSE(IsBetterTip(*first, *first));
}

TEST(BlockIndex, MoreWorkWinsHoweverLateItArrives) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* incumbent = Extend(index, index.Genesis(), 1);
    ASSERT_NE(incumbent, nullptr);
    EXPECT_EQ(index.BestHeader(), incumbent);

    const BlockIndexEntry* rival = Extend(index, index.Genesis(), 2);
    ASSERT_NE(rival, nullptr);
    const BlockIndexEntry* deeper = Extend(index, *rival, 3);
    ASSERT_NE(deeper, nullptr);

    EXPECT_EQ(index.BestHeader(), deeper);
    EXPECT_EQ(index.Size(), 4U);
}

TEST(BlockIndex, ChildrenOfReportsEveryBranchFromABlock) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* left = Extend(index, index.Genesis(), 1);
    const BlockIndexEntry* right = Extend(index, index.Genesis(), 2);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    const std::vector<const BlockIndexEntry*> children = index.ChildrenOf(index.Genesis());
    EXPECT_EQ(children.size(), 2U);
    EXPECT_NE(std::ranges::find(children, left), children.end());
    EXPECT_NE(std::ranges::find(children, right), children.end());
    EXPECT_TRUE(index.ChildrenOf(*left).empty());
}

// --- Ruling a branch out ----------------------------------------------------

TEST(BlockIndex, MarkingABlockFailedRulesOutEverythingBelowIt) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* doomed = Extend(index, index.Genesis(), 1);
    ASSERT_NE(doomed, nullptr);
    const BlockIndexEntry* below = Extend(index, *doomed, 2);
    ASSERT_NE(below, nullptr);
    const BlockIndexEntry* further = Extend(index, *below, 3);
    ASSERT_NE(further, nullptr);
    const BlockIndexEntry* survivor = Extend(index, index.Genesis(), 4);
    ASSERT_NE(survivor, nullptr);
    ASSERT_EQ(index.BestHeader(), further);

    EXPECT_EQ(index.RecordFailure(*doomed), 3U);

    // The rejected block carries its own verdict; its descendants carry an inherited one,
    // because nothing is wrong with them beyond the branch they sit on.
    EXPECT_EQ(doomed->failure, BlockFailure::Itself);
    EXPECT_EQ(below->failure, BlockFailure::Ancestor);
    EXPECT_EQ(further->failure, BlockFailure::Ancestor);
    EXPECT_FALSE(further->IsEligible());
    EXPECT_TRUE(survivor->IsEligible());

    // The tip moves to the best of what is left, without any caller having to ask.
    EXPECT_EQ(index.BestHeader(), survivor);

    // Idempotent: marking an already-failed block again marks nothing new.
    EXPECT_EQ(index.RecordFailure(*doomed), 0U);
}

TEST(BlockIndex, AHeaderOnARejectedBranchIsRefusedWithoutBeingJudged) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* doomed = Extend(index, index.Genesis(), 1);
    ASSERT_NE(doomed, nullptr);
    const BlockHeader descendant = ChildOf(*doomed, 2);
    ASSERT_EQ(index.RecordFailure(*doomed), 1U);

    const std::expected<const BlockIndexEntry*, HeaderError> refused =
        index.AddHeader(descendant, NOW, Params());
    ASSERT_FALSE(refused.has_value());
    ASSERT_TRUE(std::holds_alternative<IndexError>(refused.error()));
    EXPECT_EQ(std::get<IndexError>(refused.error()), IndexError::PredecessorInvalid);
    EXPECT_EQ(index.Size(), 2U);
}

TEST(BlockIndex, RecordedValidityOnlyEverRises) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* entry = Extend(index, index.Genesis(), 1);
    ASSERT_NE(entry, nullptr);

    index.RecordValidity(*entry, BlockValidity::Full);
    EXPECT_EQ(entry->validity, BlockValidity::Full);

    // A block re-checked after a reorganisation reports the earlier stages again; that
    // must not undo what is already known.
    index.RecordValidity(*entry, BlockValidity::Body);
    EXPECT_EQ(entry->validity, BlockValidity::Full);
}

// --- Walking between tips ---------------------------------------------------

TEST(PlanChainSwitch, AcrossAForkRevertsOneBranchAndAppliesTheOther) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* fork = Extend(index, index.Genesis(), 1);
    ASSERT_NE(fork, nullptr);
    const BlockIndexEntry* old_middle = Extend(index, *fork, 2);
    const BlockIndexEntry* old_tip = Extend(index, *old_middle, 3);
    const BlockIndexEntry* new_middle = Extend(index, *fork, 4);
    const BlockIndexEntry* new_tip = Extend(index, *new_middle, 5);
    ASSERT_NE(old_tip, nullptr);
    ASSERT_NE(new_tip, nullptr);

    const ChainSwitch plan = PlanChainSwitch(old_tip, new_tip);

    EXPECT_EQ(plan.fork_point, fork);
    // Reverting runs from the tip downwards, applying from the fork upwards: each list is
    // in the order its side must actually be performed.
    EXPECT_EQ(plan.disconnect, std::vector<const BlockIndexEntry*>({old_tip, old_middle}));
    EXPECT_EQ(plan.connect, std::vector<const BlockIndexEntry*>({new_middle, new_tip}));
}

TEST(PlanChainSwitch, AdvancingTheSameBranchRevertsNothing) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* first = Extend(index, index.Genesis(), 1);
    ASSERT_NE(first, nullptr);
    const BlockIndexEntry* second = Extend(index, *first, 2);
    const BlockIndexEntry* third = Extend(index, *second, 3);
    ASSERT_NE(third, nullptr);

    const ChainSwitch plan = PlanChainSwitch(first, third);

    EXPECT_EQ(plan.fork_point, first);
    EXPECT_TRUE(plan.disconnect.empty());
    EXPECT_EQ(plan.connect, std::vector<const BlockIndexEntry*>({second, third}));
}

TEST(PlanChainSwitch, WithNoActiveChainConnectsFromGenesis) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* first = Extend(index, index.Genesis(), 1);
    ASSERT_NE(first, nullptr);
    const BlockIndexEntry* second = Extend(index, *first, 2);
    ASSERT_NE(second, nullptr);

    // What a node does on its first start: there is no tip to leave, so the whole ancestry
    // is to be applied.
    const ChainSwitch plan = PlanChainSwitch(nullptr, second);

    EXPECT_EQ(plan.fork_point, nullptr);
    EXPECT_TRUE(plan.disconnect.empty());
    EXPECT_EQ(plan.connect, std::vector<const BlockIndexEntry*>({&index.Genesis(), first, second}));
}

// --- What a child is checked against ----------------------------------------

TEST(MedianTimePastAt, IsTheMedianOfWhatExistsNearGenesis) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    // Genesis alone: the median of one timestamp is that timestamp.
    EXPECT_EQ(MedianTimePastAt(index.Genesis()), Params().genesis_timestamp);

    const BlockIndexEntry* first = Extend(index, index.Genesis(), 1);
    ASSERT_NE(first, nullptr);
    const BlockIndexEntry* second = Extend(index, *first, 2);
    ASSERT_NE(second, nullptr);

    // Three blocks spaced by the target interval: the middle one.
    EXPECT_EQ(MedianTimePastAt(*second), first->header.timestamp);
}

TEST(NextTargetBits, RegtestNeverRetargets) {
    // Regtest difficulty is trivial by design, so its seam always answers with the
    // network floor, whatever the tip's height or timestamp claims. The second and
    // third cases build synthetic tips whose timestamps are absurdly ahead of and
    // behind schedule; a retargeting network would move difficulty for both.
    BlockIndex index = BlockIndex::ForNetwork(Params());
    EXPECT_EQ(NextTargetBits(index.Genesis(), Params()), Params().pow_limit_bits);

    const BlockIndexEntry* child = Extend(index, index.Genesis(), 1);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(NextTargetBits(*child, Params()), Params().pow_limit_bits);

    // Both height fields are set, here and below. The rule reads the *entry's* height,
    // which the index derives, while the header carries the miner's claim of it; a
    // synthetic entry that set only the header's would leave the rule looking at height
    // zero and answering with the anchor's target for every case.
    BlockIndexEntry early;
    early.height = 500;
    early.header.height = 500;
    early.header.timestamp = Params().genesis_timestamp + 500 * 300 - 172800;
    EXPECT_EQ(NextTargetBits(early, Params()), Params().pow_limit_bits);

    BlockIndexEntry late;
    late.height = 500;
    late.header.height = 500;
    late.header.timestamp = Params().genesis_timestamp + 500 * 300 + 1000000000LL;
    EXPECT_EQ(NextTargetBits(late, Params()), Params().pow_limit_bits);
}

TEST(NextTargetBits, RetargetingNetworksUseTheConsensusAsertRule) {
    // The seam is exactly the pure consensus rule fed the network's constants and
    // the tip's height and header time; a divergence here would be a consensus
    // split between the assembler and the validator.
    //
    // Each height is tried on schedule and well ahead of it. The second case is what
    // makes this test say anything: on schedule the rule answers with the anchor's own
    // target, which is also what it answers for a tip it thinks is at height zero — so
    // an on-schedule-only check would pass even if the seam never passed the height on
    // at all.
    const ChainParams* networks[] = {&MAINNET_PARAMS, &TESTNET_PARAMS};
    for (const ChainParams* params : networks) {
        for (const uint32_t height : {1U, 288U, 576U, 100000U}) {
            const int64_t on_time =
                params->genesis_timestamp + static_cast<int64_t>(height) * 300;
            for (const int64_t skew : {int64_t{0}, int64_t{-200000}}) {
                BlockIndexEntry tip;
                tip.height = height;
                tip.header.height = height;
                tip.header.timestamp = on_time + skew;
                EXPECT_EQ(NextTargetBits(tip, *params),
                          consensus::AsertNextBits(*params, height, on_time + skew))
                    << params->name << " height " << height << " skew " << skew;
            }
        }
    }

    // And the height is not merely passed but load-bearing. A chain 200,000 seconds
    // ahead of schedule at height 288 has earned a harder target than the anchor's,
    // which is the answer a tip whose height went missing would have given.
    BlockIndexEntry ahead;
    ahead.height = 288;
    ahead.header.height = 288;
    ahead.header.timestamp = MAINNET_PARAMS.genesis_timestamp + 288 * 300 - 200000;
    const uint32_t harder = NextTargetBits(ahead, MAINNET_PARAMS);
    EXPECT_NE(harder, MAINNET_PARAMS.genesis_bits);
    const std::optional<Target> harder_target = CompactToTarget(harder);
    const std::optional<Target> anchor_target = CompactToTarget(MAINNET_PARAMS.genesis_bits);
    ASSERT_TRUE(harder_target.has_value());
    ASSERT_TRUE(anchor_target.has_value());
    EXPECT_LT(*harder_target, *anchor_target);
}

TEST(NextTargetBits, TheAsertRuleIsNeverEasierThanTheFloor) {
    // A tip whose schedule position would make the next block trivially easy (the
    // chain far behind schedule) must clamp to the floor, never below it.
    BlockIndexEntry tip;
    tip.height = 1000;
    tip.header.height = 1000;
    tip.header.timestamp = MAINNET_PARAMS.genesis_timestamp + 1000 * 300 + 1000000000LL;
    const uint32_t bits = NextTargetBits(tip, MAINNET_PARAMS);
    EXPECT_EQ(bits, MAINNET_PARAMS.pow_limit_bits);
    EXPECT_TRUE(CompactToTarget(bits).has_value());
}

TEST(NextTargetBits, TestnetUsesMinimumDifficultyAfterAHashrateGap) {
    BlockIndexEntry tip;
    tip.height = 100;
    tip.header.height = 100;
    // Put the parent ahead of schedule so ordinary ASERT is visibly harder than
    // the floor; otherwise this test could pass while the escape hatch did nothing.
    tip.header.timestamp = TESTNET_PARAMS.genesis_timestamp + 100 * 300 - 100000;

    const uint32_t ordinary = NextTargetBits(tip, tip.header.timestamp + 600, TESTNET_PARAMS);
    const uint32_t escaped = NextTargetBits(tip, tip.header.timestamp + 601, TESTNET_PARAMS);
    EXPECT_EQ(escaped, TESTNET_PARAMS.pow_limit_bits);
    EXPECT_NE(ordinary, TESTNET_PARAMS.pow_limit_bits);
}

TEST(NextTargetBits, MainnetDoesNotUseTheMinimumDifficultyEscapeHatch) {
    BlockIndexEntry tip;
    tip.height = 100;
    tip.header.height = 100;
    tip.header.timestamp = MAINNET_PARAMS.genesis_timestamp + 100 * 300 - 100000;
    EXPECT_NE(NextTargetBits(tip, tip.header.timestamp + 601, MAINNET_PARAMS),
              MAINNET_PARAMS.pow_limit_bits);
}

TEST(BlockIndexEntry, AncestorWalksToAGivenHeight) {
    BlockIndex index = BlockIndex::ForNetwork(Params());
    const BlockIndexEntry* first = Extend(index, index.Genesis(), 1);
    ASSERT_NE(first, nullptr);
    const BlockIndexEntry* second = Extend(index, *first, 2);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(second->Ancestor(2), second);
    EXPECT_EQ(second->Ancestor(1), first);
    EXPECT_EQ(second->Ancestor(0), &index.Genesis());
    EXPECT_EQ(second->Ancestor(3), nullptr);
}

}  // namespace
}  // namespace amarian::chain
