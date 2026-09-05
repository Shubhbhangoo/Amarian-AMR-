#pragma once

/// \file
/// The signature hash: the exact bytes a spend signature is over.
///
/// This is the whole of spend authorisation's security. A signature is a claim about
/// a *message*, and every property anyone relies on — that the coins go where the
/// signer meant, in the amount they meant, on the network they meant, for the input
/// they meant — is a property of what went into this preimage and nothing else.
/// Anything omitted here is something an attacker may change after the fact.
///
/// ## Fixed size, and why that is a security property rather than tidiness
///
/// The per-input preimage is exactly `SIGHASH_PREIMAGE_SIZE` bytes regardless of how
/// large the transaction is, because everything about the transaction as a whole
/// enters through three 32-byte hashes computed once for the whole transaction. That
/// is the fix for a specific historical denial-of-service class: a sighash whose
/// preimage contains the whole transaction, recomputed per input, makes validation
/// cost grow with the square of transaction size. Bitcoin shipped that and could not
/// remove it. Amarian decides it before any block exists, which is the only time it
/// can be decided.
///
/// ## No flag byte
///
/// Every signature commits to everything. There is no "sign only this input" or
/// "sign none of the outputs" selector, because that family of options has a long
/// history of subtle failures and Phase 1 does not need it. If such a mode is ever
/// wanted it arrives as a new `condition_version` with its own analysis, not as a
/// byte in this preimage that every existing signature would then have to have
/// committed to.
///
/// ## Where the layering puts it
///
/// A pure function of transaction data plus three values the caller supplies, so it
/// belongs with the other canonical encodings and identifiers rather than with the
/// rules. `chain_id` and the spent amount arrive as arguments: this file therefore
/// has no dependency on the consensus parameters, and a wallet can compute the same
/// message a validator will without linking the validator.

#include <amarian/primitives/spend_condition.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace amarian {

// --- Domain tags -------------------------------------------------------------

/// The tag every signature hash is computed under.
///
/// One tag for the whole scheme, with the domain byte below separating the four
/// things hashed under it. The alternative — four tags — would have put four strings
/// into the consensus-critical tag list where one and a counter do the same work.
inline constexpr std::string_view SIGHASH_TAG = "Amarian/SigHash";

/// The tag under which a `SpendCondition` becomes the 32 bytes a version-1 lock
/// holds in its `program`.
///
/// Defined here rather than in lock.hpp because this is the file that computes it:
/// the same commitment appears in the output that created the coin and in the message
/// that authorises spending it, and computing it in one place is what makes those two
/// the same value by construction rather than by two implementations agreeing.
inline constexpr std::string_view SPEND_CONDITION_TAG = "Amarian/SpendCondition";

/// Which of the four things hashed under `SIGHASH_TAG` a given preimage is.
///
/// The three per-transaction hashes are over lists of different shapes, so a
/// cross-list collision would already be hard to produce and useless if produced —
/// each hash lands in its own fixed position in a fixed-length preimage. The byte
/// costs nothing and means the argument does not have to be made.
enum class SigHashDomain : uint8_t {
    /// Every input's `OutPoint`, in order.
    Outpoints = 1,
    /// Every input's `sequence`, in order.
    Sequences = 2,
    /// Every `TxOutput`, in order.
    Outputs = 3,
    /// The per-input message itself.
    Input = 4,
};

// --- Sizes -------------------------------------------------------------------

/// The exact length of the per-input preimage.
///
/// 1 domain + 32 chain_id + 4 version + 4 locktime + 32 outpoints + 32 sequences +
/// 32 outputs + 4 input index + 8 spent amount + 32 condition commitment.
inline constexpr size_t SIGHASH_PREIMAGE_SIZE = 181;

// --- The commitment a lock holds ---------------------------------------------

/// `TaggedHash("Amarian/SpendCondition", serialise(condition))`.
///
/// This is the 32 bytes a `LOCK_VERSION_CONDITION_COMMITMENT` output carries in its
/// `program`, and the 32 bytes the signature hash below folds in. Both call this
/// function, which is the point: the check "the revealed condition is the one this
/// output was locked to" and the message "I authorise a spend of an output locked to
/// this condition" are then the same value by construction. Two implementations of it
/// could differ, and a difference would let a signature authorise a spend of an output
/// it was not made for.
[[nodiscard]] Hash256 SpendConditionCommitment(const SpendCondition& condition);

// --- The per-transaction hashes ----------------------------------------------

/// The three hashes that are the same for every input of one transaction.
///
/// Computed once by `ComputeSigHashMidstates` and passed to `SignatureHash` for each
/// input. This split *is* the linear-cost property: validating an n-input transaction
/// serialises its outpoints, sequences and outputs once, not n times.
///
/// A `SigHashMidstates` belongs to exactly one transaction. Using one computed from a
/// different transaction produces a signature hash for that other transaction, which
/// no signature will match — a bug, not a vulnerability, but the reason the two are
/// always passed together.
struct SigHashMidstates {
    /// Every input's outpoint, count-prefixed.
    Hash256 outpoints;
    /// Every input's sequence, count-prefixed.
    Hash256 sequences;
    /// Every output in full — amount, lock version, lock program — count-prefixed.
    Hash256 outputs;

    friend bool operator==(const SigHashMidstates&, const SigHashMidstates&) noexcept = default;
};

/// Computes the three per-transaction hashes.
///
/// Reads only `inputs` and `outputs`; the witness section is deliberately not covered
/// by any of the three, because a signature cannot commit to the section that contains
/// it. Witness malleability is instead prevented by the wtxid: a third party may
/// re-encode a witness, but the result has a different wtxid and so a different Merkle
/// root, and the txid an outpoint refers to does not move.
[[nodiscard]] SigHashMidstates ComputeSigHashMidstates(const Transaction& tx);

// --- The message ------------------------------------------------------------

/// The 32-byte message the signatures in input `input_index`'s witness must be over.
///
/// `chain_id` is the network's identifying hash, which is why a mainnet signature is
/// not a valid testnet signature and a testnet coin cannot be replayed onto mainnet.
/// `spent_amount` is the amount of the output being spent, which the transaction does
/// not itself contain: committing to it is what stops a third party who knows an
/// input's signature from re-presenting it against a different, larger output of the
/// same condition, and it is what lets a hardware signer state the fee it is
/// authorising without being handed the whole UTXO set.
///
/// `condition` is the revealed spend condition for this input. It is folded in through
/// `SpendConditionCommitment`, so a signature made for a 2-of-3 cannot be replayed
/// against a 1-of-3 over the same keys.
///
/// `input_index` is committed as a `uint32_t` and is not bounds-checked here: this
/// function reads nothing out of the transaction by that index, so an out-of-range
/// value produces a message no signature matches rather than an out-of-bounds read.
/// Consensus checks the index against the witness count before it gets here.
[[nodiscard]] Hash256 SignatureHash(const Hash256& chain_id,
                                    const Transaction& tx,
                                    const SigHashMidstates& midstates,
                                    uint32_t input_index,
                                    int64_t spent_amount,
                                    const SpendCondition& condition);

/// The preimage itself, for tests and for a `-printsighash` style diagnostic.
///
/// Exposed because a signature hash that cannot be inspected is a signature hash whose
/// vectors cannot be checked against another implementation, and cross-implementation
/// agreement about this exact byte string is what stops a chain split. Always exactly
/// `SIGHASH_PREIMAGE_SIZE` bytes.
[[nodiscard]] ByteVec SignatureHashPreimage(const Hash256& chain_id,
                                            const Transaction& tx,
                                            const SigHashMidstates& midstates,
                                            uint32_t input_index,
                                            int64_t spent_amount,
                                            const SpendCondition& condition);



}  // namespace amarian
