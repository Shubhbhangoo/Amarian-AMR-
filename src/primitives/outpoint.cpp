#include <amarian/primitives/outpoint.hpp>

namespace amarian {

void OutPoint::Serialize(Writer& writer) const {
    writer.WriteHash256(txid);
    writer.WriteU32(index);
}

bool OutPoint::Deserialize(Reader& reader, OutPoint& out) noexcept {
    OutPoint decoded;
    if (!reader.ReadHash256(decoded.txid) || !reader.ReadU32(decoded.index)) {
        return false;
    }
    out = decoded;
    return true;
}

}  // namespace amarian
