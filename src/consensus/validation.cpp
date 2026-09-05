/// \file
/// The consensus rules, in the order AMARIAN_PROTOCOL.md's "Validation order" states
/// them. Where this file and that section differ, one of them is a bug.

#include <amarian/consensus/issuance.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/util/overflow.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace amarian::consensus {
namespace {

/// The reserved scheme id is declared twice — once in `primitives` for the data types
/// and once in `crypto` for the registry — because the two namespaces cannot share it
/// without primitives depending on the registry. This is the assertion that the
/// duplication stays a duplication rather than becoming a disagreement.
static_assert(amarian::SCHEME_RESERVED == crypto::SCHEME_RESERVED,
              "the reserved signature scheme id must be the same value in both layers");

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

/// Whether a key's length disagrees with what its scheme's registry entry claims.
///
/// A scheme this build has never heard of has no length to disagree with, so it is not
/// wrong — it is unknown, and is left for the soft-fork path to accept.
[[nodiscard]] bool HasWrongPublicKeySize(const PublicKey& key) noexcept {
    const crypto::SchemeSpec* spec = crypto::FindScheme(key.scheme);
    return spec != nullptr && key.bytes.size() != spec->public_key_bytes;
}

/// The same question for a signature.
[[nodiscard]] bool HasWrongSignatureSize(const Signature& signature) noexcept {
    const crypto::SchemeSpec* spec = crypto::FindScheme(signature.scheme);
    return spec != nullptr && signature.bytes.size() != spec->signature_bytes;
}

/// Whether a version-1 lock's program is exactly the commitment to `condition`.
///
/// A program of any length other than 32 cannot be a commitment, so it simply does not
/// match — there is no separate malformed-lock verdict, because a lock whose program is
/// the wrong length is a lock no condition satisfies. Both values are public, so this is
/// an ordinary comparison and not a constant-time one.
[[nodiscard]] bool MatchesConditionCommitment(const Lock& lock, const SpendCondition& condition) {
    const Hash256 commitment = SpendConditionCommitment(condition);
    return std::ranges::equal(lock.program, commitment.Span());
}

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
        case ValidationError::ConditionKeyWrongSize:
            return "public key is not the length its scheme defines";
        case ValidationError::WitnessSignatureWrongSize:
            return "signature is not the length its scheme defines";

        case ValidationError::TxCoinbaseAuthorisesNothing:
            return "coinbase has no inputs to authorise and pays no fee";
        case ValidationError::TxSpentOutputCountMismatch:
            return "not exactly one spent output was supplied per input";
        case ValidationError::TxSpendsUnspendableOutput:
            return "input spends an output locked with unspendable lock version 0";
        case ValidationError::TxConditionDoesNotMatchLock:
            return "revealed spend condition is not the one the output committed to";
        case ValidationError::TxSignatureDoesNotVerify:
            return "no key in the spend condition verifies the signature offered";
        case ValidationError::TxInputSumOutOfRange:
            return "spent amounts sum outside [0, MAX_MONEY]";
        case ValidationError::TxOutputsExceedInputs:
            return "transaction pays out more than it spends";

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
        // A key of the wrong length for a scheme this build knows can never verify
        // anything, and finding that out costs one integer comparison here instead of a
        // call into a cryptographic library later. A key under an *unknown* scheme has no
        // length to check against and is deliberately left alone.
        if (HasWrongPublicKeySize(condition.keys[index])) {
            return Reject(ValidationError::ConditionKeyWrongSize);
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

    // Signature lengths, like key lengths, only under the version this node understands.
    // A future condition version is free to define a different relationship between its
    // signatures and the registry — aggregation, for one — and this build cannot know
    // what that is, so it does not impose today's answer on it.
    if (witness.condition.version == CONDITION_VERSION_THRESHOLD) {
        for (const Signature& signature : witness.signatures) {
            if (HasWrongSignatureSize(signature)) {
                return Reject(ValidationError::WitnessSignatureWrongSize);
            }
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

// --- Spend authorisation ---------------------------------------------------------

namespace {

/// Whether `witness` satisfies its own threshold condition for `message`.
///
/// Ordered forward match: `key_index` only ever advances, so signature *i* is tried
/// against the keys left over from signature *i−1*. At most one verification happens per
/// key for the entire witness, which bounds the cost by the key count the condition's own
/// commitment fixed — and makes the ascending order the only one that verifies, so a
/// permuted witness is an invalid witness rather than a second encoding of a valid one.
[[nodiscard]] Verdict SatisfiesThreshold(const Witness& witness, const Hash256& message) {
    size_t key_index = 0;
    for (const Signature& signature : witness.signatures) {
        bool satisfied = false;
        while (key_index < witness.condition.keys.size()) {
            const PublicKey& key = witness.condition.keys[key_index];
            ++key_index;
            if (key.scheme != signature.scheme) {
                // A key for another scheme cannot answer this signature. It is consumed
                // rather than left to be reconsidered later, which is exactly what keeps
                // the walk linear and the valid ordering unique.
                continue;
            }
            switch (crypto::Verify(signature.scheme, key.bytes, signature.bytes, message)) {
                case crypto::VerifyResult::Valid:
                    satisfied = true;
                    break;
                case crypto::VerifyResult::UnknownScheme:
                    // The soft-fork path, and the one place consensus accepts something
                    // it has not checked. This build cannot verify a scheme it does not
                    // implement, and calling it a forgery would fork this node off the
                    // chain the moment the scheme was deployed. Upgraded nodes do check
                    // it; a hashpower majority enforcing a rule old nodes cannot see is
                    // what a soft fork is.
                    satisfied = true;
                    break;
                case crypto::VerifyResult::Invalid:
                case crypto::VerifyResult::Malformed:
                    // This key does not answer this signature, so the next one is tried.
                    // `Malformed` here can only be a right-length key the implementation
                    // cannot parse: skipped rather than fatal, because the keys were fixed
                    // when the coin was created and one unusable key must not make the
                    // other n−1 unspendable.
                    break;
                case crypto::VerifyResult::Reserved:
                    // Unreachable: scheme 0 is rejected structurally in both the condition
                    // and the witness, long before this. Named rather than defaulted so
                    // that a new VerifyResult is a build failure here.
                    return Reject(ValidationError::WitnessSignatureSchemeReserved);
            }
            if (satisfied) {
                break;
            }
        }
        if (!satisfied) {
            return Reject(ValidationError::TxSignatureDoesNotVerify);
        }
    }
    return Accept();
}

}  // namespace

Computed<int64_t> TransactionFee(const Transaction& tx, std::span<const TxOutput> spent_outputs) {
    if (tx.IsCoinbase()) {
        return std::unexpected(ValidationError::TxCoinbaseAuthorisesNothing);
    }
    if (spent_outputs.size() != tx.inputs.size()) {
        return std::unexpected(ValidationError::TxSpentOutputCountMismatch);
    }

    // Both sums are range-checked at every step rather than only at the end. A running
    // total that leaves the money range has already lost the property that makes the
    // comparison below meaningful, and `TryAccumulate` reports the overflow instead of
    // wrapping into a value that would look like a smaller amount.
    int64_t spent = 0;
    for (const TxOutput& output : spent_outputs) {
        if (!IsValidAmount(output.amount) || !TryAccumulate(spent, output.amount) ||
            !IsValidAmount(spent)) {
            return std::unexpected(ValidationError::TxInputSumOutOfRange);
        }
    }

    int64_t paid = 0;
    for (const TxOutput& output : tx.outputs) {
        if (!IsValidAmount(output.amount) || !TryAccumulate(paid, output.amount) ||
            !IsValidAmount(paid)) {
            return std::unexpected(ValidationError::TxOutputSumOutOfRange);
        }
    }

    // Half of the supply guarantee. No transaction may pay out more than it spends, so
    // no transaction can create value; the coinbase, which is allowed to, is bounded by
    // `CheckCoinbaseAmount` against the schedule instead.
    if (paid > spent) {
        return std::unexpected(ValidationError::TxOutputsExceedInputs);
    }
    // Cannot overflow: both are in [0, MAX_MONEY] and `paid` is no larger than `spent`.
    return spent - paid;
}

Verdict CheckSpendAuthorisation(const Transaction& tx,
                                std::span<const TxOutput> spent_outputs,
                                const ChainParams& params) {
    if (tx.IsCoinbase()) {
        return Reject(ValidationError::TxCoinbaseAuthorisesNothing);
    }
    if (spent_outputs.size() != tx.inputs.size()) {
        return Reject(ValidationError::TxSpentOutputCountMismatch);
    }
    // `CheckTransaction` already guarantees this. It is re-checked because that guarantee
    // is what stops the indexing below from running past the end of a caller's vector,
    // and a bound that memory safety depends on is not one to take on trust.
    if (tx.witnesses.size() != tx.inputs.size()) {
        return Reject(ValidationError::TxWitnessCountMismatch);
    }

    // Once for the whole transaction, not once per input. This is the entire reason
    // `SigHashMidstates` is a separate type: it makes the cost of verifying a transaction
    // linear in its size rather than quadratic, and that is a property of *where* this
    // line is, so it is worth a comment saying so.
    const SigHashMidstates midstates = ComputeSigHashMidstates(tx);

    for (size_t index = 0; index < tx.inputs.size(); ++index) {
        const TxOutput& spent = spent_outputs[index];
        const Witness& witness = tx.witnesses[index];

        if (spent.lock.IsUnspendable()) {
            return Reject(ValidationError::TxSpendsUnspendableOutput);
        }
        if (spent.lock.version != LOCK_VERSION_CONDITION_COMMITMENT) {
            // An unknown lock version has no rules in this build, so there is nothing
            // here to enforce and the output stays spendable. Not a hole: no wallet
            // creates outputs under a version that has not been defined, and once one is,
            // the upgraded majority enforces it.
            continue;
        }

        // The revealed condition must be the one the coin committed to. This is what makes
        // a version-1 lock a lock at all: 32 bytes in the output decide, by preimage
        // resistance, exactly which key set and threshold may spend it.
        if (!MatchesConditionCommitment(spent.lock, witness.condition)) {
            return Reject(ValidationError::TxConditionDoesNotMatchLock);
        }
        if (witness.condition.version != CONDITION_VERSION_THRESHOLD) {
            // The commitment above still bound the spender to the exact condition the coin
            // named, because that check hashes the serialised bytes and so does not depend
            // on the version. What an unknown version's signatures *mean* is the part this
            // build does not know, and it does not guess.
            continue;
        }

        // The spent amount is committed here, which is why a signature cannot be replayed
        // against a different output of the same condition, and why the fee a transaction
        // pays is fixed by its signatures rather than by whatever a relayer claims.
        const Hash256 message = SignatureHash(params.chain_id,
                                              tx,
                                              midstates,
                                              static_cast<uint32_t>(index),
                                              spent.amount,
                                              witness.condition);
        if (const Verdict satisfied = SatisfiesThreshold(witness, message); !satisfied) {
            return satisfied;
        }
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
