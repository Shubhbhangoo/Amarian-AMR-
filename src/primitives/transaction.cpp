#include <amarian/crypto/hash.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/overflow.hpp>

#include <utility>

namespace amarian {
namespace {

/// Reads a compact-size count, rejects it above `max`, and narrows it to size_t.
/// `min_element_bytes` bounds the count against the bytes actually remaining.
bool ReadCount(Reader& reader, size_t& out, size_t max, size_t min_element_bytes) {
    uint64_t count = 0;
    if (!reader.ReadCompactSize(count, min_element_bytes) || std::cmp_greater(count, max)) {
        reader.Fail();
        return false;
    }
    const auto narrowed = TryNarrow<size_t>(count);
    if (!narrowed.has_value()) {
        reader.Fail();
        return false;
    }
    out = *narrowed;
    return true;
}

}  // namespace

void TxInput::Serialize(Writer& writer) const {
    outpoint.Serialize(writer);
    writer.WriteU32(sequence);
}

bool TxInput::Deserialize(Reader& reader, TxInput& out) noexcept {
    TxInput decoded;
    if (!OutPoint::Deserialize(reader, decoded.outpoint) || !reader.ReadU32(decoded.sequence)) {
        return false;
    }
    out = decoded;
    return true;
}

void TxOutput::Serialize(Writer& writer) const {
    writer.WriteI64(amount);
    lock.Serialize(writer);
}

bool TxOutput::Deserialize(Reader& reader, TxOutput& out, size_t max_lock_program_size) {
    TxOutput decoded;
    if (!reader.ReadI64(decoded.amount) ||
        !Lock::Deserialize(reader, decoded.lock, max_lock_program_size)) {
        return false;
    }
    // Range check at deserialisation, per DECISIONS #20. A negative amount is
    // representable on the wire so that it is detectable; this is the one place
    // the detection happens, and every amount in memory has passed it.
    if (decoded.amount < 0 || decoded.amount > MAX_MONEY) {
        reader.Fail();
        return false;
    }
    out = std::move(decoded);
    return true;
}

void Transaction::Serialize(Writer& writer) const {
    SerializeWithoutWitnesses(writer);
    writer.WriteCompactSize(witnesses.size());
    for (const Witness& witness : witnesses) {
        witness.Serialize(writer);
    }
}

void Transaction::SerializeWithoutWitnesses(Writer& writer) const {
    writer.WriteU32(version);
    writer.WriteCompactSize(inputs.size());
    for (const TxInput& input : inputs) {
        input.Serialize(writer);
    }
    writer.WriteCompactSize(outputs.size());
    for (const TxOutput& output : outputs) {
        output.Serialize(writer);
    }
    writer.WriteU32(locktime);
}

bool Transaction::Deserialize(Reader& reader, Transaction& out, const TxLimits& limits) {
    Transaction decoded;

    if (!reader.ReadU32(decoded.version)) {
        return false;
    }

    // A TxInput is exactly 40 bytes, so its count is bounded against the bytes
    // remaining before anything is reserved.
    size_t input_count = 0;
    if (!ReadCount(reader, input_count, limits.max_inputs, 40)) {
        return false;
    }
    decoded.inputs.reserve(input_count);
    for (size_t index = 0; index < input_count; ++index) {
        TxInput input;
        if (!TxInput::Deserialize(reader, input)) {
            return false;
        }
        decoded.inputs.push_back(std::move(input));
    }

    // A TxOutput is at least 10 bytes: 8 for the amount, 1 for the lock version,
    // 1 for the program's compact-size length.
    size_t output_count = 0;
    if (!ReadCount(reader, output_count, limits.max_outputs, 10)) {
        return false;
    }
    decoded.outputs.reserve(output_count);
    for (size_t index = 0; index < output_count; ++index) {
        TxOutput output;
        if (!TxOutput::Deserialize(reader, output, limits.max_lock_program_size)) {
            return false;
        }
        decoded.outputs.push_back(std::move(output));
    }

    if (!reader.ReadU32(decoded.locktime)) {
        return false;
    }

    // A Witness is at least 4 bytes: 3 for an empty condition, 1 for a
    // zero-signature count. Its nested lengths are bounded as they are read.
    size_t witness_count = 0;
    if (!ReadCount(reader, witness_count, limits.max_witnesses, 4)) {
        return false;
    }
    decoded.witnesses.reserve(witness_count);
    for (size_t index = 0; index < witness_count; ++index) {
        Witness witness;
        if (!Witness::Deserialize(reader,
                                  witness,
                                  limits.max_keys,
                                  limits.max_key_size,
                                  limits.max_signatures,
                                  limits.max_signature_size)) {
            return false;
        }
        decoded.witnesses.push_back(std::move(witness));
    }

    out = std::move(decoded);
    return true;
}

Hash256 Transaction::Txid() const {
    Writer writer;
    SerializeWithoutWitnesses(writer);
    return DoubleSha256(writer.Bytes());
}

Hash256 Transaction::Wtxid() const {
    Writer writer;
    Serialize(writer);
    return DoubleSha256(writer.Bytes());
}

}  // namespace amarian
