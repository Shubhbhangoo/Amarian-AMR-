#include <amarian/primitives/witness.hpp>
#include <amarian/util/overflow.hpp>

#include <utility>

namespace amarian {

void Witness::Serialize(Writer& writer) const {
    condition.Serialize(writer);
    writer.WriteCompactSize(signatures.size());
    for (const Signature& signature : signatures) {
        signature.Serialize(writer);
    }
}

bool Witness::Deserialize(Reader& reader,
                          Witness& out,
                          size_t max_keys,
                          size_t max_key_size,
                          size_t max_signatures,
                          size_t max_signature_size) {
    Witness decoded;
    if (!SpendCondition::Deserialize(reader, decoded.condition, max_keys, max_key_size)) {
        return false;
    }

    uint64_t count = 0;
    // A signature always contains a u16 scheme and one compact-size byte.
    if (!reader.ReadCompactSize(count, 3) || std::cmp_greater(count, max_signatures)) {
        reader.Fail();
        return false;
    }
    const auto count_size = TryNarrow<size_t>(count);
    if (!count_size.has_value()) {
        reader.Fail();
        return false;
    }
    decoded.signatures.reserve(*count_size);
    for (size_t index = 0; index < *count_size; ++index) {
        Signature signature;
        if (!Signature::Deserialize(reader, signature, max_signature_size)) {
            return false;
        }
        decoded.signatures.push_back(std::move(signature));
    }
    out = std::move(decoded);
    return true;
}

}  // namespace amarian
