#pragma once

/// \file
/// The consensus rules that decide whether a transaction or a block is valid.
///
/// This is the surface that decides which chain is real, so three properties are
/// deliberate and worth stating before the declarations:
///
/// **Every check is a pure function of its explicit inputs.** No globals, no ambient
/// "current network", no clock read from inside a rule. A rule that needs the time or
/// the predecessor's timestamps is handed them, which is what makes every one of these
/// reproducible: the same inputs give the same verdict on every node, forever.
///
/// **Failure names the rule that fired.** `ValidationError` is an enumeration rather
/// than a string, so it allocates nothing on a path an attacker can drive, and so a
/// test can assert on the exact rule rather than on prose. `Describe` turns one into
/// text at the edges — logs and RPC — and nowhere else.
///
/// **Order is part of the specification.** Cheap and context-free checks run before
/// expensive and contextual ones, because the order decides how much work an attacker
/// can make a node do before their input is rejected. The order here is the order in
/// AMARIAN_PROTOCOL.md's "Validation order" section, and the two are meant to be read
/// together.
///
/// ## What is not here yet
///
/// The contextual transaction rules — the outpoint exists and is unspent, coinbase
/// maturity, the revealed condition hashes to the lock's commitment, inputs cover
/// outputs, and signature verification — need the UTXO set and the signature scheme
/// registry, which are the next two components. Their absence is why `CheckBlock`
/// below is explicit that it is the context-free half: a block that passes it is not
/// yet valid, and no caller should be able to read the name and think otherwise.

#include <amarian/consensus/params.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/spend_condition.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/primitives/witness.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace amarian::consensus {

/// The rule that rejected a transaction or block.
///
/// One enumerator per rule, never a shared "invalid" value: the point of naming them
/// is that a node operator reading a log, and a test asserting on a verdict, both learn
/// which rule fired rather than that something did. Values are not serialised and carry
/// no wire meaning, so they may be reordered.
enum class ValidationError : uint16_t {
    // --- Header, and the work it claims ------------------------------------------
    HeaderTargetMalformed,       ///< `target_bits` is not a valid compact encoding.
    HeaderTargetBelowFloor,      ///< Easier than the network's proof-of-work floor.
    HeaderInsufficientWork,      ///< The hash does not meet the target it claims.
    HeaderWrongHeight,           ///< Not `prev.height + 1`.
    HeaderWrongPrevBlock,        ///< Does not name the predecessor it is offered for.
    HeaderUnexpectedTarget,      ///< Not the target this node itself computes for the height.
    HeaderTimestampTooOld,       ///< At or before the median of the last `MEDIAN_TIME_SPAN`.
    HeaderTimestampTooFarAhead,  ///< More than `MAX_FUTURE_BLOCK_SECONDS` past the clock.

    // --- Transaction, in isolation ------------------------------------------------
    TxNoInputs,
    TxNoOutputs,
    TxDuplicateInput,       ///< The same outpoint appears twice in one transaction.
    TxNullOutpoint,         ///< A non-coinbase input naming the coinbase sentinel.
    TxAmountOutOfRange,     ///< An output amount outside `[0, MAX_MONEY]`.
    TxOutputSumOutOfRange,  ///< The outputs sum above `MAX_MONEY`.
    TxWeightTooLarge,       ///< Heavier than a whole block may be.
    TxTooManyInputs,
    TxTooManyOutputs,
    TxWitnessCountMismatch,       ///< Not exactly one witness per input.
    TxCoinbaseHasWitnesses,       ///< A coinbase authorises nothing, so it carries none.
    TxCoinbaseDataTooLarge,       ///< Beyond `MAX_COINBASE_DATA_BYTES`.
    TxCoinbaseDataOnNonCoinbase,  ///< Bytes that would never be serialised, so never signed.

    // --- Spend conditions and witnesses -------------------------------------------
    ConditionVersionReserved,  ///< Version 0 is reserved and can never be satisfied.
    ConditionNoKeys,
    ConditionTooManyKeys,
    ConditionKeysNotStrictlyOrdered,  ///< Not ascending, or a key appears twice.
    ConditionKeySchemeReserved,       ///< Scheme 0 is reserved and can never verify.
    ConditionThresholdZero,           ///< Nothing to satisfy is not the same as anyone-can-spend.
    ConditionThresholdAboveKeyCount,  ///< Unsatisfiable, and so a lock nobody can open.
    WitnessSignatureCountMismatch,    ///< Not exactly `threshold` signatures.
    WitnessSignatureSchemeReserved,

    // --- Block ---------------------------------------------------------------------
    BlockNoTransactions,
    BlockTooManyTransactions,
    BlockFirstTxNotCoinbase,
    BlockMultipleCoinbases,
    BlockWeightTooLarge,
    BlockMerkleRootMismatch,
    BlockCoinbaseWrongHeight,  ///< The coinbase input's `sequence` is not the header's height.
    BlockDuplicateSpend,       ///< Two transactions in the block spend one outpoint.
    BlockCoinbasePaysTooMuch,  ///< Above the scheduled reward plus the fees actually paid.
};

/// A rule verdict: success, or the one rule that rejected the input.
///
/// `std::expected<void, ValidationError>` rather than a bool-and-out-parameter so that
/// the reason cannot be dropped by accident, and rather than a `None` enumerator so
/// that "there was no error" is not a value a caller can forget to test for. Four bytes
/// and trivially copyable: nothing here allocates.
using Verdict = std::expected<void, ValidationError>;

[[nodiscard]] inline Verdict Accept() noexcept {
    return Verdict{};
}

[[nodiscard]] inline Verdict Reject(ValidationError error) noexcept {
    return Verdict{std::unexpect, error};
}

/// A short stable description, for logs and RPC. Never parsed by anything.
[[nodiscard]] std::string_view Describe(ValidationError error) noexcept;

// --- Spend conditions and witnesses ---------------------------------------------
//
// Structure only. Whether the signatures actually verify is the crypto layer's answer
// and comes later in the order, because it is the one step whose cost an attacker can
// raise substantially.

/// The structural rules on a revealed spend condition: a defined version, a key list
/// that is non-empty, bounded, strictly ascending, and free of the reserved scheme, and
/// a threshold that at least one key set can satisfy.
///
/// An *unknown* `condition_version` passes: like an unknown lock version, it must stay
/// valid-and-spendable so a future condition form can be deployed by soft fork without
/// splitting old nodes off the chain. Version 0 is not unknown — it is reserved, and it
/// is rejected so that a zero-filled condition is never satisfiable.
[[nodiscard]] Verdict CheckSpendCondition(const SpendCondition& condition,
                                          const ChainParams& params);

/// The structural rules on one witness: its condition is well-formed, and it carries
/// exactly `threshold` signatures, none under the reserved scheme.
///
/// Exactly, not at least: a witness with spare signatures is a witness with spare
/// bytes, and bytes that consensus ignores are bytes an attacker can vary to change a
/// wtxid — and, since fees are paid by weight, bytes someone else pays for.
[[nodiscard]] Verdict CheckWitness(const Witness& witness, const ChainParams& params);

// --- Transactions ----------------------------------------------------------------

/// Everything about one transaction that needs no UTXO set and no enclosing block.
///
/// Deliberately does *not* reject an unknown transaction version, an unknown lock
/// version, or an unknown key scheme. Each is an extension point, and rejecting an
/// unrecognised value at any of them turns every future upgrade into a hard fork.
[[nodiscard]] Verdict CheckTransaction(const Transaction& tx, const ChainParams& params);

// --- Headers ---------------------------------------------------------------------

/// The header rules that need nothing but the header: its target is a valid compact
/// encoding, is no easier than the network's floor, and its hash meets it.
///
/// This is the cheapest gate a node has against a flood of fabricated headers — one
/// hash and one comparison — so it runs before anything that touches the block index.
[[nodiscard]] Verdict CheckBlockHeader(const BlockHeader& header, const ChainParams& params);

/// What a header must be checked against, supplied explicitly.
///
/// A struct rather than a pointer into a block index, because these five facts are all
/// a header rule needs and passing them by value keeps every rule below a pure
/// function. The block index's job is to produce this; deciding validity from it is
/// this file's job, and the two stay separable — and separately testable — because the
/// boundary is a value.
struct HeaderContext {
    /// The predecessor's hash and height.
    Hash256 prev_hash;
    uint32_t prev_height;

    /// Median timestamp of the last `MEDIAN_TIME_SPAN` blocks ending at the
    /// predecessor. A header's timestamp must be strictly greater than this.
    int64_t median_time_past;

    /// The `target_bits` this node computes for the new block from the chain itself.
    /// The header's claim is checked against it: a miner does not get to choose the
    /// difficulty they mined at.
    uint32_t expected_bits;

    /// The node's current wall-clock time, in seconds since the Unix epoch. An input
    /// rather than a call to a clock, so that the rule is reproducible and a test does
    /// not have to wait for real time to pass.
    int64_t now;
};

/// The header rules that need the predecessor: height, linkage, the target this node
/// expects, and the two timestamp bounds.
///
/// Assumes `CheckBlockHeader` has already passed — the two are separate because the
/// context-free half can be applied to a header offered by a peer before the node has
/// found, or even accepted, the block it claims to build on.
[[nodiscard]] Verdict ContextualCheckBlockHeader(const BlockHeader& header,
                                                 const HeaderContext& context,
                                                 const ChainParams& params);

/// The median of the timestamps of up to `MEDIAN_TIME_SPAN` recent blocks, newest
/// last, for `HeaderContext::median_time_past`.
///
/// Only the last `MEDIAN_TIME_SPAN` entries are used, so a caller may pass a longer
/// run without trimming it. An empty span has no median and yields `INT64_MIN`, which
/// is the value that makes the rule vacuous — correct for genesis, which has no
/// predecessors to be later than.
[[nodiscard]] int64_t MedianTimePast(std::span<const int64_t> timestamps) noexcept;

// --- Blocks ----------------------------------------------------------------------

/// Everything about a block that needs no UTXO set and no predecessor: its work, its
/// weight, its coinbase's shape and position, its commitment to its transactions, and
/// each transaction in isolation.
///
/// Named for what it is. A block that passes this is *not* valid: the contextual rules
/// — every input's outpoint exists and is unspent, maturity, signatures, and the
/// coinbase's amount against the fees actually paid — are still to come, and they are
/// the ones that need state. Nothing here is allowed to imply otherwise.
[[nodiscard]] Verdict CheckBlock(const Block& block, const ChainParams& params);

/// The supply cap: a block's coinbase may claim the scheduled reward for its height
/// plus the fees its transactions actually paid, and not one facet more.
///
/// `total_fees` is a parameter because computing it needs the UTXO set, and this rule
/// does not: given the fees, the check is arithmetic that every node performs
/// identically from the height alone. That is the whole of the supply guarantee —
/// there is no running total to trust, no operator setting, and no authority that can
/// mint. Callers pass 0 for a block that pays no fees.
[[nodiscard]] Verdict
CheckCoinbaseAmount(const Block& block, int64_t total_fees, const ChainParams& params);

}  // namespace amarian::consensus
