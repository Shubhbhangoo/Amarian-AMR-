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
/// Every transaction rule is now here, including the two that need the UTXO set: an
/// input's outpoint must exist and be unspent, and a coinbase output must have matured.
/// The first is a *lookup* and so cannot be a function of values — its failure is the
/// absence of a coin — and the `utxo` layer reports it with `TxInputMissingOrSpent` from
/// this enumeration, so that the vocabulary of rules stays in one place even though the
/// lookup does not. The second is `CheckTransactionInputs` below, which is a pure
/// function of the coins it is handed.
///
/// What is genuinely absent is anything that needs a *chain*: which of two valid branches
/// has more work, and therefore which UTXO set is the real one. That is the block index's
/// question, not a rule's.
///
/// That is why `CheckBlock` is explicit that it is the context-free half: a block that
/// passes it is not yet valid, and no caller should be able to read the name and think
/// otherwise.

#include <amarian/consensus/params.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/coin.hpp>
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
    ConditionKeyWrongSize,      ///< A known scheme with a key of the wrong length.
    WitnessSignatureWrongSize,  ///< A known scheme with a signature of the wrong length.

    // --- Spend authorisation: the inputs against the outputs they spend -----------
    TxCoinbaseAuthorisesNothing,  ///< A coinbase spends nothing, so it has no fee and no
                                  ///< input to authorise. Its reward is bounded instead
                                  ///< by `CheckCoinbaseAmount`.
    TxSpentOutputCountMismatch,   ///< Not exactly one spent coin per input.
    TxSpendsUnspendableOutput,    ///< Lock version 0, which no witness can satisfy.
    TxConditionDoesNotMatchLock,  ///< The revealed condition is not the one committed to.
    TxSignatureDoesNotVerify,     ///< No remaining key in the condition verifies it.
    TxInputSumOutOfRange,         ///< The spent amounts sum outside `[0, MAX_MONEY]`.
    TxOutputsExceedInputs,        ///< Spends more than it takes in, which would mint coins.

    // --- The UTXO set: what exists, and what has aged ------------------------------
    TxInputMissingOrSpent,      ///< The outpoint is not in the UTXO set. Reported by the
                                ///< `utxo` layer, because absence is a failed lookup
                                ///< rather than a property of any value.
    TxCoinbaseNotMature,        ///< A coinbase output spent fewer than
                                ///< `coinbase_maturity` blocks after it was created.
    TxCreatesExistingOutpoint,  ///< An output whose outpoint is already unspent. It
                                ///< would overwrite a live coin, which is coin
                                ///< destruction rather than creation.

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
    BlockFeesOutOfRange,       ///< The block's fees sum outside `[0, MAX_MONEY]`.
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
/// A key under a scheme this build *knows* must also be exactly that scheme's length.
/// The check belongs here, in the cheap context-free pass, rather than at verification
/// time: a wrong-length key can be rejected by comparing two integers, and doing it here
/// means an attacker cannot make a node reach the cryptography with a key that could
/// never have parsed. A key under an unknown scheme has no length this build can check
/// and is left alone.
///
/// An *unknown* `condition_version` passes: like an unknown lock version, it must stay
/// valid-and-spendable so a future condition form can be deployed by soft fork without
/// splitting old nodes off the chain. Version 0 is not unknown — it is reserved, and it
/// is rejected so that a zero-filled condition is never satisfiable.
[[nodiscard]] Verdict CheckSpendCondition(const SpendCondition& condition,
                                          const ChainParams& params);

/// The structural rules on one witness: its condition is well-formed, and it carries
/// exactly `threshold` signatures, none under the reserved scheme, each of its own
/// scheme's length where this build knows that scheme.
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

// --- Spend authorisation ---------------------------------------------------------
//
// The rules that need the coins being spent, and nothing else. They are separated from
// the lookup that *finds* those coins on purpose: locating them is a database question,
// and deciding whether they may be spent is arithmetic and cryptography. Splitting them
// means the expensive half is a pure function of values, which is what lets it be tested
// exhaustively without a database and, later, run on several threads without a lock.
//
// The boundary is `std::span<const Coin>` — values the caller already holds, not a handle
// it could query. A `Coin` rather than a bare `TxOutput` because two of these rules need
// facts about a coin's *creation* that the output itself does not carry: the height it was
// created at and whether it came from a coinbase.

/// A value, or the one rule that rejected the input. The `Verdict` of a rule that has
/// something to say when it succeeds.
template <typename T>
using Computed = std::expected<T, ValidationError>;

/// The fee a transaction pays: the sum of what it spends minus the sum of what it pays
/// out, rejecting any transaction that would mint coins.
///
/// The supply guarantee has exactly two halves and this is one of them. No transaction
/// may create value, and the one that is allowed to — the coinbase — is bounded instead
/// by `CheckCoinbaseAmount`, which is handed the total of the fees this function returns.
/// Between them there is no path by which a facet comes into existence unscheduled.
///
/// A coinbase spends nothing and pays no fee, so it is rejected here rather than given a
/// special case: callers sum this over the non-coinbase transactions of a block.
[[nodiscard]] Computed<int64_t> TransactionFee(const Transaction& tx,
                                               std::span<const Coin> spent_coins);

/// Whether each input is actually authorised to spend the coin it names: the revealed
/// condition is the one that coin's lock committed to, and the signatures satisfy it.
///
/// `spent_coins[i]` is the coin that `tx.inputs[i]` spends; the count is checked
/// rather than assumed, because a caller that got it wrong would otherwise read past the
/// end of the span. Whether each of those coins *exists* and is unspent is the UTXO
/// set's answer and is not asked here.
///
/// Assumes `CheckTransaction` has passed, so the witness count matches the input count
/// and every witness is structurally sound. Rejects a coinbase, which authorises nothing.
///
/// The three extension points keep their meaning under a soft fork:
///
///  - An **unknown lock version** is spendable by any witness, without a signature check.
///    Its rules do not exist yet, so there is nothing for this build to enforce; a node
///    that rejected it would fork itself off the chain the moment the rules were defined.
///  - An **unknown key scheme** counts as satisfied. An old node cannot check a signature
///    under a scheme it has never heard of, and the alternative — calling it a forgery —
///    is the same self-fork. What keeps this safe is that upgraded nodes do check it, and
///    a majority of hashpower enforcing the new rule is what a soft fork *is*.
///  - **Version 0**, at either point, is reserved and always rejected.
///
/// Signatures satisfy a threshold by **ordered forward match**: one key index advances
/// monotonically across the whole signature list, so signature *i* is checked against
/// keys from wherever signature *i−1* stopped. Two properties follow, and both are the
/// reason for choosing it over trying every pair.
///
/// The first is cost. At most `keys.size()` verifications happen no matter how many
/// signatures are offered, so the work an input can demand is bounded by the condition
/// its own commitment named — and since a threshold condition is capped at
/// `MAX_SPEND_CONDITION_KEYS`, so is the work. Trying every pair would be quadratic in a
/// value the spender chooses, which is a denial-of-service vector paid for at linear
/// weight.
///
/// The second is malleability. Exactly one ordering of a given set of signatures
/// verifies: the ascending one. Any permutation of a valid witness is an invalid witness,
/// so a relayer cannot reorder signatures to produce a second wtxid for one transaction.
///
/// A key the implementation cannot parse — right length for its scheme, but not a valid
/// point or encoding — is skipped like one whose signature simply failed, not treated as
/// a fatal defect. The condition's keys were fixed when the coin was created, and
/// rejecting outright would make an *n*-key condition unspendable because of one bad key
/// that could never have verified anything anyway. Skipping leaves it spendable by the
/// keys that do work, which is what its owner asked for.
[[nodiscard]] Verdict CheckSpendAuthorisation(const Transaction& tx,
                                              std::span<const Coin> spent_coins,
                                              const ChainParams& params);

/// Everything a non-coinbase transaction must satisfy against the coins it spends, and
/// the fee it pays if it does. The whole of the UTXO-dependent rule set except the lookup
/// itself.
///
/// `spend_height` is the height of the block this transaction is being validated *into* —
/// not the height of any coin it spends. For a transaction under consideration for the
/// mempool it is the height the next block would have, because that is the earliest block
/// it could appear in.
///
/// The order the rules run in is deliberate, and it is cheapest-first:
///
///  1. A coinbase is rejected. It spends nothing, so none of the rest applies.
///  2. The span length must equal the input count, so nothing below can read past its end.
///  3. **Maturity**: a coin from a coinbase must be at least `params.coinbase_maturity`
///     blocks old. Two integer comparisons per input, and no adversary can make them
///     expensive.
///  4. **The fee**: integer addition, and it is what rejects a transaction that tries to
///     mint. Running it before the signature checks means a would-be minter's forgery
///     costs this node no elliptic-curve work at all.
///  5. **Authorisation**: the cryptography, last, once everything an attacker can make
///     cheap to reject has already rejected it.
///
/// Maturity uses `int64_t` arithmetic on both heights so that a coin claiming a height
/// above the spending block's cannot wrap around into looking mature. Semantics match
/// Bitcoin's: a coinbase output created in block *H* is first spendable in block
/// *H + coinbase_maturity*.
[[nodiscard]] Computed<int64_t> CheckTransactionInputs(const Transaction& tx,
                                                      std::span<const Coin> spent_coins,
                                                      uint32_t spend_height,
                                                      const ChainParams& params);

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
