/// \file
/// The signature hash's implementation: four tagged hashes, one of them per input.

#include <amarian/primitives/sighash.hpp>

#include <amarian/crypto/hash.hpp>
#include <amarian/util/serialize.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace amarian {
namespace {

/// Starts a preimage with its domain byte. Every hash under `SIGHASH_TAG` begins
/// with one, so no two of the four can ever be the same byte string.
[[nodiscard]] Writer BeginPreimage(SigHashDomain domain, size_t reserve_hint) {
    Writer writer(reserve_hint);
    writer.WriteU8(static_cast<uint8_t>(domain));
    return writer;
}

}  // namespace

Hash256 SpendConditionCommitment(const SpendCondition& condition) {
    Writer writer;
    condition.Serialize(writer);
    return TaggedHash(SPEND_CONDITION_TAG, writer.Bytes());
}

SigHashMidstates ComputeSigHashMidstates(const Transaction& tx) {
    SigHashMidstates midstates{};

    // The counts are committed even though outpoints and sequences are fixed-width and
    // so already unambiguous. Uniformity is worth more here than the three bytes: the
    // outputs list is *not* fixed-width, and a rule stated as "every list commits its
    // length" is one a reviewer can check without first working out which lists need
    // it. The same reasoning as the Merkle root's count commitment.
    {
        Writer writer = BeginPreimage(SigHashDomain::Outpoints,
                                      1 + 9 + tx.inputs.size() * OutPoint::SERIALIZED_SIZE);
        writer.WriteCompactSize(tx.inputs.size());
        for (const TxInput& input : tx.inputs) {
            input.outpoint.Serialize(writer);
        }
        midstates.outpoints = TaggedHash(SIGHASH_TAG, writer.Bytes());
    }

    {
        Writer writer = BeginPreimage(SigHashDomain::Sequences,
                                      1 + 9 + tx.inputs.size() * sizeof(uint32_t));
        writer.WriteCompactSize(tx.inputs.size());
        for (const TxInput& input : tx.inputs) {
            writer.WriteU32(input.sequence);
        }
        midstates.sequences = TaggedHash(SIGHASH_TAG, writer.Bytes());
    }

    {
        Writer writer = BeginPreimage(SigHashDomain::Outputs,
                                      1 + 9 + tx.outputs.size() * TxOutput::MIN_SERIALIZED_SIZE);
        writer.WriteCompactSize(tx.outputs.size());
        for (const TxOutput& output : tx.outputs) {
            output.Serialize(writer);
        }
        midstates.outputs = TaggedHash(SIGHASH_TAG, writer.Bytes());
    }

    return midstates;
}

ByteVec SignatureHashPreimage(const Hash256& chain_id,
                              const Transaction& tx,
                              const SigHashMidstates& midstates,
                              uint32_t input_index,
                              int64_t spent_amount,
                              const SpendCondition& condition) {
    Writer writer = BeginPreimage(SigHashDomain::Input, SIGHASH_PREIMAGE_SIZE);

    // Network first, so that the very first thing a verifier on the wrong chain reads
    // is the thing that makes the message wrong.
    writer.WriteHash256(chain_id);

    // The transaction as a whole: what it is, when it may be included, what it spends,
    // and what it pays.
    writer.WriteU32(tx.version);
    writer.WriteU32(tx.locktime);
    writer.WriteHash256(midstates.outpoints);
    writer.WriteHash256(midstates.sequences);
    writer.WriteHash256(midstates.outputs);

    // This input in particular: which one it is, what it is worth, and what authorises
    // it. Without the index, a signature valid for one input of a transaction that
    // spends two outputs of the same condition would be valid for the other.
    writer.WriteU32(input_index);
    writer.WriteI64(spent_amount);
    writer.WriteHash256(SpendConditionCommitment(condition));

    // The fixed length is the denial-of-service property, so it is asserted where it is
    // produced rather than only stated in the header.
    assert(writer.Size() == SIGHASH_PREIMAGE_SIZE);
    return writer.Take();
}

Hash256 SignatureHash(const Hash256& chain_id,
                      const Transaction& tx,
                      const SigHashMidstates& midstates,
                      uint32_t input_index,
                      int64_t spent_amount,
                      const SpendCondition& condition) {
    const ByteVec preimage = SignatureHashPreimage(
        chain_id, tx, midstates, input_index, spent_amount, condition);
    return TaggedHash(SIGHASH_TAG, preimage);
}

}  // namespace amarian
