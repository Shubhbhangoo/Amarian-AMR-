#pragma once

/// \file
/// Transactions and their canonical encodings and identifiers.

#include <amarian/primitives/amount.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/primitives/outpoint.hpp>
#include <amarian/primitives/witness.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace amarian {

/// The base-size multiplier in the weight formula `base × 4 + witness`.
///
/// This is what makes post-quantum authorisation affordable: a 3 732-byte ML-DSA-44
/// input contributes 3 732 to weight rather than 14 928. The constant lives with the
/// encoding it describes, because weight is a property of how a transaction
/// serialises. The *limit* it is compared against is a chain parameter and lives with
/// the other consensus rules.
inline constexpr size_t WITNESS_SCALE_FACTOR = 4;

/// Resource bounds for decoding one transaction.
///
/// Every bound is a parameter because none is an intrinsic property of a
/// transaction: a node parsing a block, a relay node, and a wallet reconstructing
/// its own spending may legitimately admit different sizes. The count bounds are
/// checked before any allocation, in the Reader's usual style.
struct TxLimits {
    size_t max_inputs;
    size_t max_outputs;
    size_t max_witnesses;
    size_t max_lock_program_size;
    size_t max_keys;                // per spend condition
    size_t max_key_size;            // per public key
    size_t max_signatures;          // per witness
    size_t max_signature_size;      // per signature
    size_t max_coinbase_data_size;  // the coinbase's arbitrary bytes
};

/// The outpoint index a coinbase input carries.
///
/// A coinbase input references no earlier output, so its outpoint must be
/// unmistakably not an output reference: an all-zero txid alone is not enough,
/// because output 0 of the all-zero txid is a syntactically ordinary reference. The
/// pair (all-zero txid, `0xFFFFFFFF`) is what marks a transaction as creating coins
/// rather than moving them, and no valid outpoint can collide with it.
inline constexpr uint32_t COINBASE_OUTPOINT_INDEX = 0xFFFF'FFFFU;

/// A transaction input's reference and reserved sequence field. Whether an
/// outpoint exists, is unspent, or is the coinbase sentinel is a consensus rule.
struct TxInput {
    /// Fixed on the wire: an outpoint and a 4-byte sequence. Because it is fixed, a
    /// declared input count can be checked exactly against the bytes remaining.
    static constexpr size_t SERIALIZED_SIZE = OutPoint::SERIALIZED_SIZE + sizeof(uint32_t);

    OutPoint outpoint;

    /// Reserved for every input except a coinbase's, where consensus requires it to
    /// equal the block height.
    ///
    /// That requirement is what actually closes the duplicate-coinbase problem. Height
    /// lives in the block header, which the coinbase transaction does not contain, so
    /// two coinbases mined at different heights in the same issuance era — same
    /// reward, same output lock — would otherwise serialise identically and share a
    /// txid. Binding the height into the one input field that is already inside the
    /// txid preimage makes every coinbase txid distinct by construction, with no
    /// extra bytes and without a rule that has to look outside the transaction.
    uint32_t sequence = 0;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, TxInput& out) noexcept;

    friend constexpr bool operator==(const TxInput&, const TxInput&) noexcept = default;
};

/// An output amount and its versioned ownership lock.
///
/// The amount is range-checked into `[0, MAX_MONEY]` here, at deserialisation
/// (DECISIONS #20): a value that exists in memory has already been checked, so no
/// use site can miss the check.
struct TxOutput {
    /// Smallest wire form: an 8-byte amount, a lock version, and a compact-size
    /// zero-length program.
    static constexpr size_t MIN_SERIALIZED_SIZE = sizeof(int64_t) + 2;

    int64_t amount = 0;
    Lock lock;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool
    Deserialize(Reader& reader, TxOutput& out, size_t max_lock_program_size);

    friend bool operator==(const TxOutput&, const TxOutput&) noexcept = default;
};

/// A whole transaction, including the coinbase structural exception.
///
/// Decoding is canonical and bounded but does not judge: whether there is at
/// least one input and one output, whether witnesses number one per input, and
/// whether a witness's signatures satisfy its condition are consensus rules and
/// belong to the consensus layer. A malformed-in-that-sense transaction still
/// decodes, so that a node can reject it *as* malformed rather than as
/// undecodable.
///
/// ## The coinbase's shape
///
/// A coinbase creates coins instead of spending them, so it has nothing to
/// authorise and no signature to carry. In its witness section's place it carries one
/// arbitrary byte string — the extranonce, and in genesis the timestamped reference.
///
/// This is a tagged union whose tag is the input itself: `IsCoinbase()` is decidable
/// from the inputs, which the wire format places *before* the witness section, so a
/// decoder always knows which form follows without looking outside the transaction
/// and without consulting the block that contains it. A non-coinbase transaction that
/// forged the tag would decode, and would then be rejected by consensus for spending
/// an outpoint that cannot exist — which is the layering this codec keeps everywhere:
/// decode says what the bytes are, consensus says whether they are allowed.
struct Transaction {
    /// Smallest wire form: a 4-byte version, three compact-size zero counts, and a
    /// 4-byte locktime. A block's transaction count is bounded against this before
    /// anything is reserved, and it is the floor the per-block transaction limit is
    /// derived from.
    static constexpr size_t MIN_SERIALIZED_SIZE = sizeof(uint32_t) + 3 + sizeof(uint32_t);

    uint32_t version = 0;
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;
    uint32_t locktime = 0;

    /// Authorisation, one witness per input. Empty in a coinbase.
    std::vector<Witness> witnesses;

    /// The coinbase's arbitrary bytes, in place of its witness list. Ignored — and so
    /// not serialised at all — unless `IsCoinbase()`.
    ByteVec coinbase_data;

    /// Whether this transaction creates coins: exactly one input, whose outpoint is
    /// the all-zero txid at index `COINBASE_OUTPOINT_INDEX`.
    ///
    /// This is a structural question, answerable from the transaction alone, and it
    /// decides which wire form the witness section takes. Whether such a transaction
    /// is *permitted* — at index 0 of a block, once, paying no more than the
    /// scheduled reward plus fees — is a consensus rule elsewhere.
    [[nodiscard]] bool IsCoinbase() const noexcept {
        return inputs.size() == 1 && inputs[0].outpoint.txid.IsZero() &&
               inputs[0].outpoint.index == COINBASE_OUTPOINT_INDEX;
    }

    /// Full canonical form, witness section included: the wtxid preimage.
    void Serialize(Writer& writer) const;

    /// The txid preimage: version, inputs, outputs, locktime, never the witness
    /// section. For a coinbase that means the extranonce is outside the txid and
    /// inside the wtxid, so rolling it changes the Merkle root — which is what makes
    /// it useful for splitting work — while leaving the txid stable.
    void SerializeWithoutWitnesses(Writer& writer) const;

    /// Leaves `out` untouched on failure.
    [[nodiscard]] static bool Deserialize(Reader& reader, Transaction& out, const TxLimits& limits);

    /// Transaction ID: double SHA-256 over the witness-free serialisation.
    /// Stable regardless of how the spend was authorised, so it is the value an
    /// outpoint refers to and witness malleability cannot change it.
    [[nodiscard]] Hash256 Txid() const;

    /// Witness transaction ID: double SHA-256 over the whole serialisation,
    /// witnesses included. This is what the Merkle tree commits to.
    [[nodiscard]] Hash256 Wtxid() const;

    /// `base_size × 4 + witness_size`, the transaction's own contribution to its
    /// block's weight.
    ///
    /// Measured from the encodings rather than from a parallel size calculation, for
    /// the same reason `Block::Weight` is: a second implementation of "how long is
    /// this once serialised" is a second place for the answer to be wrong, and weight
    /// decides what a block may contain.
    [[nodiscard]] size_t Weight() const;

    friend bool operator==(const Transaction&, const Transaction&) noexcept = default;
};

/// The single input a coinbase carries: the sentinel outpoint, with the block height
/// in `sequence`.
///
/// One constructor, used by the genesis builder, by block assembly, and by the
/// validator's expectation of what a coinbase input must look like. Three
/// hand-written copies of a sentinel are three chances for one of them to differ.
[[nodiscard]] inline TxInput MakeCoinbaseInput(uint32_t height) noexcept {
    return TxInput{.outpoint = OutPoint{.txid = Hash256{}, .index = COINBASE_OUTPOINT_INDEX},
                   .sequence = height};
}

}  // namespace amarian
