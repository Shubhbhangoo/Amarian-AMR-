/// \file
/// The consensus rules, in the order AMARIAN_PROTOCOL.md's "Validation order" states
/// them. Where this file and that section differ, one of them is a bug.

#include <amarian/consensus/issuance.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/util/overflow.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace amarian::consensus {
namespace {

/// Odd 64-bit multiplier — the golden ratio's reciprocal scaled to 2^64 — so that the
/// outpoint index below influences every bit of the mix rather than only the low ones.
constexpr size_t OUTPOINT_INDEX_MULTIPLIER = 0x9E37'79B9'7F4A'7C15ULL;

/// Hashes an outpoint for the duplicate-spend sets below.
///
/// A txid is already a uniformly distributed 256-bit value, so eight of its bytes
/// mixed with the index is as good a hash as anything derived from it — and, unlike a
/// generic combiner, it cannot be made to collide by an attacker choosing txids,
/// because choosing a txid means finding a preimage.
struct OutPointHash {
    [[nodiscard]] size_t operator()(const OutPoint& outpoint) const noexcept {
        static_assert(sizeof(size_t) >= sizeof(uint64_t),
                      "the mixing below assumes a 64-bit hash word");
        const std::array<uint8_t, Hash256::SIZE>& bytes = outpoint.txid.Array();
        size_t mixed = 0;
        for (size_t index = 0; index < sizeof(uint64_t); ++index) {
            mixed |= size_t{bytes[index]} << (index * 8);
        }
        mixed ^= size_t{outpoint.index} * OUTPOINT_INDEX_MULTIPLIER;
        return mixed;
    }
};

using OutPointSet = std::unordered_set<OutPoint, OutPointHash>;

}  // namespace

std::string_view Describe(ValidationError error) noexcept {
    switch (error) {
        case ValidationError::HeaderTargetMalformed:
            return "target_bits is not a valid compact target encoding";
        case ValidationError::HeaderTargetBelowFloor:
            return "target is easier than this network's proof-of-work floor";
        case ValidationError::HeaderInsufficientWork:
            return "block hash does not meet the target the header claims";
        case ValidationError::HeaderWrongHeight:
            return "height is not the predecessor's height plus one";
        case ValidationError::HeaderWrongPrevBlock:
            return "prev_block does not name the predecessor";
        case ValidationError::HeaderUnexpectedTarget:
            return "target_bits is not the target this node computes for this height";
        case ValidationError::HeaderTimestampTooOld:
            return "timestamp is not after the median of the preceding blocks";
        case ValidationError::HeaderTimestampTooFarAhead:
            return "timestamp is too far ahead of this node's clock";

        case ValidationError::TxNoInputs:
            return "transaction has no inputs";
        case ValidationError::TxNoOutputs:
            return "transaction has no outputs";
        case ValidationError::TxDuplicateInput:
            return "transaction spends the same outpoint twice";
        case ValidationError::TxNullOutpoint:
            return "non-coinbase input names the coinbase sentinel outpoint";
        case ValidationError::TxAmountOutOfRange:
            return "output amount is outside [0, MAX_MONEY]";
        case ValidationError::TxOutputSumOutOfRange:
            return "outputs sum above MAX_MONEY";
        case ValidationError::TxWeightTooLarge:
            return "transaction is heavier than a whole block may be";
        case ValidationError::TxTooManyInputs:
            return "transaction has more inputs than consensus permits";
        case ValidationError::TxTooManyOutputs:
            return "transaction has more outputs than consensus permits";
        case ValidationError::TxWitnessCountMismatch:
            return "transaction does not carry exactly one witness per input";
        case ValidationError::TxCoinbaseHasWitnesses:
            return "coinbase carries witnesses, which it authorises nothing with";
        case ValidationError::TxCoinbaseDataTooLarge:
            return "coinbase data exceeds the permitted size";
        case ValidationError::TxCoinbaseDataOnNonCoinbase:
            return "non-coinbase transaction carries coinbase data";

        case ValidationError::ConditionVersionReserved:
            return "spend condition version 0 is reserved and can never be satisfied";
        case ValidationError::ConditionNoKeys:
            return "spend condition has no keys";
        case ValidationError::ConditionTooManyKeys:
            return "spend condition has more keys than consensus permits";
        case ValidationError::ConditionKeysNotStrictlyOrdered:
            return "spend condition keys are not in strictly ascending order";
        case ValidationError::ConditionKeySchemeReserved:
            return "spend condition uses reserved key scheme 0";
        case ValidationError::ConditionThresholdZero:
            return "spend condition threshold is zero";
        case ValidationError::ConditionThresholdAboveKeyCount:
            return "spend condition threshold exceeds its key count";
        case ValidationError::WitnessSignatureCountMismatch:
            return "witness does not carry exactly threshold signatures";
        case ValidationError::WitnessSignatureSchemeReserved:
            return "witness uses reserved signature scheme 0";

        case ValidationError::BlockNoTransactions:
            return "block has no transactions";
        case ValidationError::BlockTooManyTransactions:
            return "block has more transactions than consensus permits";
        case ValidationError::BlockFirstTxNotCoinbase:
            return "block's first transaction is not a coinbase";
        case ValidationError::BlockMultipleCoinbases:
            return "block contains more than one coinbase";
        case ValidationError::BlockWeightTooLarge:
            return "block weight exceeds the limit";
        case ValidationError::BlockMerkleRootMismatch:
            return "merkle_root does not commit to the block's transactions";
        case ValidationError::BlockCoinbaseWrongHeight:
            return "coinbase input's sequence is not the header's height";
        case ValidationError::BlockDuplicateSpend:
            return "two transactions in the block spend the same outpoint";
        case ValidationError::BlockCoinbasePaysTooMuch:
            return "coinbase claims more than the scheduled reward plus fees";
    }
    // Unreachable for any enumerator: the switch above is total, and -Wswitch-enum
    // makes a newly added one a build failure rather than a silent fall-through.
    return "unknown validation error";
}

// --- Spend conditions and witnesses ---------------------------------------------

Verdict CheckSpendCondition(const SpendCondition& condition, const ChainParams& params) {
    if (condition.version == CONDITION_VERSION_RESERVED) {
        return Reject(ValidationError::ConditionVersionReserved);
    }
    if (condition.version != CONDITION_VERSION_THRESHOLD) {
        // An unknown version stays valid-and-spendable so that a future condition form
        // can be soft-forked in. Its structure is whatever that version defines, so
        // there is nothing here for this node to check.
        return Accept();
    }

    if (condition.keys.empty()) {
        return Reject(ValidationError::ConditionNoKeys);
    }
    if (condition.keys.size() > params.block_limits.tx.max_keys) {
        return Reject(ValidationError::ConditionTooManyKeys);
    }
    if (condition.threshold == 0) {
        return Reject(ValidationError::ConditionThresholdZero);
    }
    if (size_t{condition.threshold} > condition.keys.size()) {
        return Reject(ValidationError::ConditionThresholdAboveKeyCount);
    }

    // Strictly ascending, which is one comparison serving two rules: sorted makes the
    // encoding canonical, and distinct stops a threshold from being met by counting one
    // key twice. The order is `PublicKey`'s own `<=>`, so this rule and any code that
    // builds a condition cannot disagree about what sorted means.
    for (size_t index = 0; index < condition.keys.size(); ++index) {
        if (condition.keys[index].scheme == SCHEME_RESERVED) {
            return Reject(ValidationError::ConditionKeySchemeReserved);
        }
        if (index > 0 && !(condition.keys[index - 1] < condition.keys[index])) {
            return Reject(ValidationError::ConditionKeysNotStrictlyOrdered);
        }
    }
    return Accept();
}

Verdict CheckWitness(const Witness& witness, const ChainParams& params) {
    if (const Verdict condition = CheckSpendCondition(witness.condition, params); !condition) {
        return condition;
    }

    // Exactly `threshold`, not at least: spare signatures are bytes consensus ignores,
    // which makes them bytes an attacker can vary to change a wtxid, and — since fees
    // are paid by weight — bytes somebody else pays for.
    //
    // Only checked for the version this node understands. An unknown version defines
    // its own relationship between signatures and threshold, and imposing this one on
    // it would be exactly the hard fork that leaving unknown versions alone avoids.
    if (witness.condition.version == CONDITION_VERSION_THRESHOLD &&
        witness.signatures.size() != size_t{witness.condition.threshold}) {
        return Reject(ValidationError::WitnessSignatureCountMismatch);
    }

    // The reserved scheme is rejected under every condition version, known or not: it
    // is reserved so that a zero-filled or truncated-then-padded field never names a
    // scheme, and that guarantee is worth nothing if a future version can waive it.
    for (const Signature& signature : witness.signatures) {
        if (signature.scheme == SCHEME_RESERVED) {
            return Reject(ValidationError::WitnessSignatureSchemeReserved);
        }
    }
    return Accept();
}

// --- Transactions ----------------------------------------------------------------

Verdict CheckTransaction(const Transaction& tx, const ChainParams& params) {
    const TxLimits& limits = params.block_limits.tx;

    if (tx.inputs.empty()) {
        return Reject(ValidationError::TxNoInputs);
    }
    if (tx.outputs.empty()) {
        return Reject(ValidationError::TxNoOutputs);
    }
    if (tx.inputs.size() > limits.max_inputs) {
        return Reject(ValidationError::TxTooManyInputs);
    }
    if (tx.outputs.size() > limits.max_outputs) {
        return Reject(ValidationError::TxTooManyOutputs);
    }

    const bool is_coinbase = tx.IsCoinbase();

    if (is_coinbase) {
        if (!tx.witnesses.empty()) {
            return Reject(ValidationError::TxCoinbaseHasWitnesses);
        }
        if (tx.coinbase_data.size() > limits.max_coinbase_data_size) {
            return Reject(ValidationError::TxCoinbaseDataTooLarge);
        }
    } else {
        // Data that would never be serialised is data no identifier commits to and no
        // signature covers, so a transaction carrying it is not the transaction it
        // appears to be. Rejected rather than ignored.
        if (!tx.coinbase_data.empty()) {
            return Reject(ValidationError::TxCoinbaseDataOnNonCoinbase);
        }
        if (tx.witnesses.size() != tx.inputs.size()) {
            return Reject(ValidationError::TxWitnessCountMismatch);
        }
        for (const TxInput& input : tx.inputs) {
            // The sentinel outpoint names an output that cannot exist, so an input
            // claiming it in a transaction that is not a coinbase is rejected here
            // rather than carried as far as a UTXO lookup. A transaction with one such
            // input *is* a coinbase by structure and took the branch above; this
            // catches the case where it is one of several.
            if (input.outpoint.txid.IsZero() && input.outpoint.index == COINBASE_OUTPOINT_INDEX) {
                return Reject(ValidationError::TxNullOutpoint);
            }
        }
    }

    // Amounts before either of the two checks below, because this is pure arithmetic
    // over values already in hand: it allocates nothing and serialises nothing.
    //
    // Deserialisation already range-checks each amount, but a transaction may also have
    // been built in memory — by mining, by a wallet, by a test — and this is the layer
    // that decides validity, so the verdict does not depend on how the value arrived.
    int64_t total = 0;
    for (const TxOutput& output : tx.outputs) {
        if (!IsValidAmount(output.amount)) {
            return Reject(ValidationError::TxAmountOutOfRange);
        }
        if (!TryAccumulate(total, output.amount) || !IsValidAmount(total)) {
            return Reject(ValidationError::TxOutputSumOutOfRange);
        }
    }

    if (!is_coinbase) {
        // Duplicate outpoints within one transaction, rejected here rather than left to
        // the UTXO set: spending the same output twice in one transaction would
        // otherwise be caught or missed depending on the order the inputs are applied
        // in, which is not a property consensus may have.
        OutPointSet seen;
        seen.reserve(tx.inputs.size());
        for (const TxInput& input : tx.inputs) {
            if (!seen.insert(input.outpoint).second) {
                return Reject(ValidationError::TxDuplicateInput);
            }
        }

        for (const Witness& witness : tx.witnesses) {
            if (const Verdict verdict = CheckWitness(witness, params); !verdict) {
                return verdict;
            }
        }
    }

    // Last, because it is the only context-free check that serialises the transaction.
    if (tx.Weight() > params.max_block_weight) {
        return Reject(ValidationError::TxWeightTooLarge);
    }
    return Accept();
}

// --- Headers ---------------------------------------------------------------------

Verdict CheckBlockHeader(const BlockHeader& header, const ChainParams& params) {
    const std::optional<Target> target = CompactToTarget(header.target_bits);
    if (!target.has_value()) {
        return Reject(ValidationError::HeaderTargetMalformed);
    }

    // A target *above* the floor is less work than the network requires. Compared as
    // 256-bit big-endian byte strings, which is what a `Target` is, so the comparison
    // is the array's own lexicographic order and needs no bignum.
    const std::optional<Target> floor = CompactToTarget(params.pow_limit_bits);
    if (!floor.has_value()) {
        // Only reachable from a corrupted parameter table, and a node that cannot
        // determine its own floor must not accept work at all.
        return Reject(ValidationError::HeaderTargetMalformed);
    }
    if (*target > *floor) {
        return Reject(ValidationError::HeaderTargetBelowFloor);
    }

    if (!HashMeetsTarget(header.Hash(), *target)) {
        return Reject(ValidationError::HeaderInsufficientWork);
    }
    return Accept();
}

int64_t MedianTimePast(std::span<const int64_t> timestamps) noexcept {
    if (timestamps.empty()) {
        return std::numeric_limits<int64_t>::min();
    }
    const std::span<const int64_t> window_span =
        timestamps.last(std::min(timestamps.size(), MEDIAN_TIME_SPAN));

    // Copied because the median is found by partial reordering and the caller's block
    // index is not this function's to permute.
    std::vector<int64_t> window(window_span.begin(), window_span.end());
    // nth_element rather than a full sort: the median is the only order statistic this
    // needs, and the window is walked once per block.
    const auto middle = window.begin() + static_cast<std::ptrdiff_t>(window.size() / 2);
    std::nth_element(window.begin(), middle, window.end());
    return *middle;
}

Verdict ContextualCheckBlockHeader(const BlockHeader& header,
                                   const HeaderContext& context,
                                   const ChainParams& params) {
    if (header.prev_block != context.prev_hash) {
        return Reject(ValidationError::HeaderWrongPrevBlock);
    }
    // The predecessor's height is a u32, so a height one past its maximum cannot be
    // represented: the successor of the last representable height does not exist, and
    // this is checked rather than allowed to wrap to zero.
    if (context.prev_height == std::numeric_limits<uint32_t>::max() ||
        header.height != context.prev_height + 1) {
        return Reject(ValidationError::HeaderWrongHeight);
    }
    // The node's own recomputation, not the miner's claim: difficulty is not a field a
    // block gets to choose.
    if (header.target_bits != context.expected_bits) {
        return Reject(ValidationError::HeaderUnexpectedTarget);
    }

    // Strictly after the median, so equal is rejected and time cannot be held still.
    if (header.timestamp <= context.median_time_past) {
        return Reject(ValidationError::HeaderTimestampTooOld);
    }
    const std::optional<int64_t> ceiling = CheckedAdd(context.now, MAX_FUTURE_BLOCK_SECONDS);
    if (!ceiling.has_value() || header.timestamp > *ceiling) {
        return Reject(ValidationError::HeaderTimestampTooFarAhead);
    }

    // `params` carries no rule this function needs yet: both timestamp bounds are
    // network-independent, and the per-network parts of difficulty are already resolved
    // into `context.expected_bits` by whoever computed it. It stays in the signature
    // because every other rule in this file takes it, and because the version and
    // deployment rules that will live here do need it.
    (void)params;
    return Accept();
}

// --- Blocks ----------------------------------------------------------------------

Verdict CheckBlock(const Block& block, const ChainParams& params) {
    // Work first. It is one hash and one comparison, and it is the only check that
    // costs an attacker more to pass than it costs this node to run — so everything
    // below is work a peer has already paid for.
    if (const Verdict header = CheckBlockHeader(block.header, params); !header) {
        return header;
    }

    if (block.transactions.empty()) {
        return Reject(ValidationError::BlockNoTransactions);
    }
    if (block.transactions.size() > params.block_limits.max_transactions) {
        return Reject(ValidationError::BlockTooManyTransactions);
    }

    // Exactly one coinbase, at index 0. Both halves matter: a block with none creates
    // no coins and has nowhere to pay fees, and a block with two would pay twice.
    // Cheap enough — one structural test per transaction, no serialising and no
    // hashing — to come before weight.
    if (!block.transactions.front().IsCoinbase()) {
        return Reject(ValidationError::BlockFirstTxNotCoinbase);
    }
    for (size_t index = 1; index < block.transactions.size(); ++index) {
        if (block.transactions[index].IsCoinbase()) {
            return Reject(ValidationError::BlockMultipleCoinbases);
        }
    }

    // The coinbase input's `sequence` carries the height, which is what makes every
    // coinbase txid distinct. `IsCoinbase()` above guarantees the single input this
    // reads. Checked against the header the coinbase arrived in, so the rule needs no
    // chain context.
    if (block.transactions.front().inputs.front().sequence != block.header.height) {
        return Reject(ValidationError::BlockCoinbaseWrongHeight);
    }

    // Weight before the Merkle root, and before validating any transaction, because it
    // is the bound on how much work either of those can be made to do.
    if (block.Weight() > params.max_block_weight) {
        return Reject(ValidationError::BlockWeightTooLarge);
    }

    // The commitment to the transactions, before validating them: a block whose header
    // does not commit to the bytes it carries is not the block its hash names, and
    // there is no reason to spend anything validating those bytes.
    const std::optional<Hash256> root = block.ComputeMerkleRoot();
    if (!root.has_value() || *root != block.header.merkle_root) {
        return Reject(ValidationError::BlockMerkleRootMismatch);
    }

    for (const Transaction& tx : block.transactions) {
        if (const Verdict verdict = CheckTransaction(tx, params); !verdict) {
            return verdict;
        }
    }

    // One outpoint may be spent once in the whole block, not once per transaction.
    // Without this, two transactions in one block could each spend the same output and
    // both would pass in isolation.
    OutPointSet spent;
    for (const Transaction& tx : block.transactions) {
        if (tx.IsCoinbase()) {
            continue;
        }
        for (const TxInput& input : tx.inputs) {
            if (!spent.insert(input.outpoint).second) {
                return Reject(ValidationError::BlockDuplicateSpend);
            }
        }
    }
    return Accept();
}

Verdict CheckCoinbaseAmount(const Block& block, int64_t total_fees, const ChainParams& params) {
    if (block.transactions.empty() || !block.transactions.front().IsCoinbase()) {
        return Reject(ValidationError::BlockFirstTxNotCoinbase);
    }
    // A fee total outside the money range is a caller that has already lost track of
    // the arithmetic, and this rule will not launder it into a permitted reward.
    if (!IsValidAmount(total_fees)) {
        return Reject(ValidationError::TxAmountOutOfRange);
    }

    int64_t claimed = 0;
    for (const TxOutput& output : block.transactions.front().outputs) {
        if (!IsValidAmount(output.amount) || !TryAccumulate(claimed, output.amount) ||
            !IsValidAmount(claimed)) {
            return Reject(ValidationError::TxOutputSumOutOfRange);
        }
    }

    // The whole of the supply guarantee: the reward comes from the height by the same
    // schedule on every node, and the ceiling is that plus the fees the block actually
    // paid. Claiming less is allowed — those coins are simply never created.
    const int64_t reward = BlockReward(block.header.height, params.issuance);
    const std::optional<int64_t> permitted = CheckedAdd(reward, total_fees);
    if (!permitted.has_value() || claimed > *permitted) {
        return Reject(ValidationError::BlockCoinbasePaysTooMuch);
    }
    return Accept();
}

}  // namespace amarian::consensus
