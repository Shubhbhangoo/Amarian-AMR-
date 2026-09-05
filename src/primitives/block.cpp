#include <amarian/crypto/hash.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/merkle.hpp>
#include <amarian/util/overflow.hpp>

#include <utility>

namespace amarian {

void BlockHeader::Serialize(Writer& writer) const {
    writer.WriteU32(version);
    writer.WriteU32(height);
    writer.WriteHash256(prev_block);
    writer.WriteHash256(merkle_root);
    writer.WriteI64(timestamp);
    writer.WriteU32(target_bits);
    writer.WriteU64(nonce);
}

bool BlockHeader::Deserialize(Reader& reader, BlockHeader& out) noexcept {
    BlockHeader decoded;
    if (!reader.ReadU32(decoded.version) || !reader.ReadU32(decoded.height) ||
        !reader.ReadHash256(decoded.prev_block) || !reader.ReadHash256(decoded.merkle_root) ||
        !reader.ReadI64(decoded.timestamp) || !reader.ReadU32(decoded.target_bits) ||
        !reader.ReadU64(decoded.nonce)) {
        return false;
    }
    out = decoded;
    return true;
}

Hash256 BlockHeader::Hash() const {
    Writer writer(SERIALIZED_SIZE);
    Serialize(writer);
    return DoubleSha256(writer.Bytes());
}

void Block::Serialize(Writer& writer) const {
    header.Serialize(writer);
    writer.WriteCompactSize(transactions.size());
    for (const Transaction& transaction : transactions) {
        transaction.Serialize(writer);
    }
}

bool Block::Deserialize(Reader& reader, Block& out, const BlockLimits& limits) {
    Block decoded;
    if (!BlockHeader::Deserialize(reader, decoded.header)) {
        return false;
    }

    uint64_t count = 0;
    // The count is bounded against the bytes actually remaining, using the smallest
    // a transaction can be, before anything is reserved.
    if (!reader.ReadCompactSize(count, Transaction::MIN_SERIALIZED_SIZE) ||
        std::cmp_greater(count, limits.max_transactions)) {
        reader.Fail();
        return false;
    }
    const auto narrowed = TryNarrow<size_t>(count);
    if (!narrowed.has_value()) {
        reader.Fail();
        return false;
    }

    decoded.transactions.reserve(*narrowed);
    for (size_t index = 0; index < *narrowed; ++index) {
        Transaction transaction;
        if (!Transaction::Deserialize(reader, transaction, limits.tx)) {
            return false;
        }
        decoded.transactions.push_back(std::move(transaction));
    }

    out = std::move(decoded);
    return true;
}

std::optional<Hash256> Block::ComputeMerkleRoot() const {
    std::vector<Hash256> wtxids;
    wtxids.reserve(transactions.size());
    for (const Transaction& transaction : transactions) {
        wtxids.push_back(transaction.Wtxid());
    }
    return amarian::ComputeMerkleRoot(wtxids);
}

size_t Block::Weight() const {
    // Derived from the encodings themselves rather than from a parallel size
    // calculation. A second implementation of "how long is this once serialised" is a
    // second place for the answer to be wrong, and weight decides whether a block is
    // admissible.
    //
    // Everything outside the transactions is base size, so it is scaled; each
    // transaction contributes its own weight, which already distinguishes its base
    // bytes from its witness bytes.
    size_t weight =
        (BlockHeader::SERIALIZED_SIZE + CompactSizeLen(transactions.size())) * WITNESS_SCALE_FACTOR;
    for (const Transaction& transaction : transactions) {
        weight += transaction.Weight();
    }
    return weight;
}

}  // namespace amarian
