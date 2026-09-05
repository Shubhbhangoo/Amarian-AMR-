#include <amarian/primitives/spend_condition.hpp>
#include <amarian/util/overflow.hpp>

#include <utility>

namespace amarian {

void PublicKey::Serialize(Writer& writer) const {
    writer.WriteU16(scheme);
    writer.WriteByteString(bytes);
}

bool PublicKey::Deserialize(Reader& reader, PublicKey& out, size_t max_key_size) {
    PublicKey decoded;
    if (!reader.ReadU16(decoded.scheme) || !reader.ReadByteString(decoded.bytes, max_key_size)) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

void Signature::Serialize(Writer& writer) const {
    writer.WriteU16(scheme);
    writer.WriteByteString(bytes);
}

bool Signature::Deserialize(Reader& reader, Signature& out, size_t max_signature_size) {
    Signature decoded;
    if (!reader.ReadU16(decoded.scheme) ||
        !reader.ReadByteString(decoded.bytes, max_signature_size)) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

void SpendCondition::Serialize(Writer& writer) const {
    writer.WriteU8(version);
    writer.WriteU8(threshold);
    writer.WriteCompactSize(keys.size());
    for (const PublicKey& key : keys) {
        key.Serialize(writer);
    }
}

bool SpendCondition::Deserialize(Reader& reader,
                                 SpendCondition& out,
                                 size_t max_keys,
                                 size_t max_key_size) {
    SpendCondition decoded;
    if (!reader.ReadU8(decoded.version) || !reader.ReadU8(decoded.threshold)) {
        return false;
    }

    uint64_t count = 0;
    // A key always contains a u16 scheme and one compact-size byte.
    if (!reader.ReadCompactSize(count, PublicKey::MIN_SERIALIZED_SIZE) ||
        std::cmp_greater(count, max_keys)) {
        reader.Fail();
        return false;
    }
    const auto count_size = TryNarrow<size_t>(count);
    if (!count_size.has_value()) {
        reader.Fail();
        return false;
    }
    decoded.keys.reserve(*count_size);
    for (size_t index = 0; index < *count_size; ++index) {
        PublicKey key;
        if (!PublicKey::Deserialize(reader, key, max_key_size)) {
            return false;
        }
        decoded.keys.push_back(std::move(key));
    }
    out = std::move(decoded);
    return true;
}

}  // namespace amarian
