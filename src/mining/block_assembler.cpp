#include <amarian/mining/block_assembler.hpp>

#include <amarian/chain/block_index.hpp>
#include <amarian/consensus/issuance.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/types.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace amarian::mining {

std::string_view Describe(TemplateError error) noexcept {
    switch (error) {
        case TemplateError::CoinbaseDataTooLarge:
            return "the miner's coinbase data is longer than a block may carry";
        case TemplateError::HeightOverflow:
            return "the chain is at the highest height a header can express";
        case TemplateError::MalformedTarget:
            return "this build's chain parameters produced an unusable target";
        case TemplateError::NoMerkleRoot:
            return "the assembled block committed to no transactions";
        case TemplateError::HeaderRefused:
            return "this node's own header rules refused the assembled header";
        case TemplateError::RewardRefused:
            return "this node's own supply rule refused the assembled reward";
    }
    // Unreachable: the switch is total, and -Wswitch-enum makes a new enumerator a build
    // failure here rather than a silent fall-through.
    return "unknown template error";
}

std::expected<BlockTemplate, TemplateError>
BuildBlockTemplate(const chain::BlockIndexEntry& tip, const Lock& payout, int64_t now,
                   ByteVec coinbase_data, const ChainParams& params) {
    // Refused before anything is built. `CheckTransaction` would reject the assembled block
    // for the same reason, and truncating instead would hand back a template committing to
    // bytes the miner did not choose.
    if (coinbase_data.size() > params.block_limits.tx.max_coinbase_data_size) {
        return std::unexpected(TemplateError::CoinbaseDataTooLarge);
    }
    if (tip.height == std::numeric_limits<uint32_t>::max()) {
        return std::unexpected(TemplateError::HeightOverflow);
    }
    const uint32_t height = tip.height + 1;

    // The node's own computation, never a caller's preference.
    // `ContextualCheckBlockHeader` recomputes this and compares, so the only compact target
    // that can appear in a block this node accepts is the one this line produces.
    const uint32_t bits = chain::NextTargetBits(tip, params);
    const std::optional<Target> target = CompactToTarget(bits);
    if (!target.has_value()) {
        return std::unexpected(TemplateError::MalformedTarget);
    }

    BlockTemplate assembled;
    assembled.target = *target;

    // The scheduled issuance for this height, and no fee term because a template is
    // coinbase-only until there is a mempool. Zero is what `CheckCoinbaseAmount` is handed
    // below, rather than something it assumes, so the day fees exist an expression here
    // changes and no rule anywhere does.
    assembled.reward = BlockReward(height, params.issuance);

    Transaction coinbase;
    coinbase.version = 1;
    // The height travels in the input's `sequence`, which consensus requires and which is
    // also what keeps every coinbase txid distinct without a single extra byte: two blocks
    // at different heights paying the same lock the same amount still commit to different
    // transactions. `MakeCoinbaseInput` is the one place that shape is written down.
    coinbase.inputs.push_back(MakeCoinbaseInput(height));
    coinbase.outputs.push_back(TxOutput{.amount = assembled.reward, .lock = payout});
    coinbase.locktime = 0;
    // Outside the txid and inside the wtxid, so rolling it changes the Merkle root and the
    // work being searched without changing who is paid what.
    coinbase.coinbase_data = std::move(coinbase_data);

    Block& block = assembled.block;
    block.transactions.push_back(std::move(coinbase));

    block.header.version = 1;
    block.header.height = height;
    block.header.prev_block = tip.hash;

    // Strictly after the median of the eleven blocks ending at the tip, which is what
    // `HeaderTimestampTooOld` demands. Raised to satisfy it rather than the clock being
    // trusted blindly: a tip whose timestamps run ahead of this machine is a chain this node
    // has already accepted, so the fault is the clock's and the next block must still be
    // mineable. Never lowered — a clock far *ahead* of the network produces a block peers
    // refuse until it is not, and that is a fault to fix on this machine.
    const int64_t earliest = chain::MedianTimePastAt(tip);
    // Saturating rather than adding blindly. A median at the very top of the range has no
    // valid child at all, and that is for the header check below to report rather than for
    // an overflow here to turn into a timestamp before the epoch.
    const int64_t lower_bound =
        earliest < std::numeric_limits<int64_t>::max() ? earliest + 1 : earliest;
    block.header.timestamp = std::max(now, lower_bound);

    block.header.target_bits = bits;

    // The root the transactions actually produce, asked for rather than tracked. A recorded
    // root would be a second description of the block's contents, and two descriptions of
    // one thing are two things that can disagree.
    const std::optional<Hash256> root = block.ComputeMerkleRoot();
    if (!root.has_value()) {
        return std::unexpected(TemplateError::NoMerkleRoot);
    }
    block.header.merkle_root = *root;

    // Zero, and left there. The nonce is the only field a solver moves, and starting every
    // template from zero is what makes `SolveHeader`'s contract — continue from where the
    // header stands — mean something for a caller that resumes a search.
    block.header.nonce = 0;

    // Judged by the rules, before a single hash is spent on it. Both calls are
    // proof-of-work-independent by construction: `ContextualCheckBlockHeader` covers the
    // height, the linkage, the target and both timestamp bounds, and `CheckCoinbaseAmount`
    // covers the supply. What is deliberately *not* called here is `CheckBlock`, whose
    // header half includes `HeaderInsufficientWork` — an unsolved template fails that by
    // definition, so the whole block is checked later, once it is solved, on its way through
    // `ChainState::AcceptBlock`.
    const consensus::HeaderContext context = chain::HeaderContextFor(tip, now, params);
    if (const consensus::Verdict placed =
            consensus::ContextualCheckBlockHeader(block.header, context, params);
        !placed) {
        return std::unexpected(TemplateError::HeaderRefused);
    }
    if (const consensus::Verdict paid = consensus::CheckCoinbaseAmount(block, 0, params);
        !paid) {
        return std::unexpected(TemplateError::RewardRefused);
    }
    return assembled;
}

bool SolveHeader(BlockHeader& header, const Target& target, uint64_t attempts) noexcept {
    for (uint64_t tried = 0; tried < attempts; ++tried) {
        if (HashMeetsTarget(header.Hash(), target)) {
            return true;
        }
        if (header.nonce == std::numeric_limits<uint64_t>::max()) {
            // The range is exhausted rather than wrapped. Wrapping would re-search nonces
            // already known to fail, forever, with no way for a caller to tell that from a
            // search still making progress. Rolling `coinbase_data` into a fresh template is
            // the way on, and that is the caller's call to make.
            return false;
        }
        ++header.nonce;
    }
    // The first untried nonce is what is left behind, so a caller that calls again resumes
    // without re-hashing anything it has already rejected. `attempts` is therefore a budget
    // and not a range: n calls of one attempt search exactly what one call of n attempts
    // searches.
    return false;
}

}  // namespace amarian::mining
