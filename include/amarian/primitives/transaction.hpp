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
    size_t max_keys;            // per spend condition
    size_t max_key_size;        // per public key
    size_t max_signatures;      // per witness
    size_t max_signature_size;  // per signature
};

/// A transaction input's reference and reserved sequence field. Whether an
/// outpoint exists, is unspent, or is the coinbase sentinel is a consensus rule.
struct TxInput {
    OutPoint outpoint;
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
    int64_t amount = 0;
    Lock lock;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool
    Deserialize(Reader& reader, TxOutput& out, size_t max_lock_program_size);

    friend bool operator==(const TxOutput&, const TxOutput&) noexcept = default;
};

/// A whole transaction, excluding only the coinbase structural exception.
///
/// Decoding is canonical and bounded but does not judge: whether there is at
/// least one input and one output, whether witnesses number one per input, and
/// whether a witness's signatures satisfy its condition are consensus rules and
/// belong to the consensus layer. A malformed-in-that-sense transaction still
/// decodes, so that a node can reject it *as* malformed rather than as
/// undecodable.
struct Transaction {
    uint32_t version = 0;
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;
    uint32_t locktime = 0;
    std::vector<Witness> witnesses;

    /// Full canonical form, witnesses included: the wtxid preimage.
    void Serialize(Writer& writer) const;

    /// The txid preimage: version, inputs, outputs, locktime, never witnesses.
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

    friend bool operator==(const Transaction&, const Transaction&) noexcept = default;
};

}  // namespace amarian
