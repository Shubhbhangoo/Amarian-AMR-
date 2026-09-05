#include <amarian/primitives/coin.hpp>

#include <utility>

namespace amarian {

void Coin::Serialize(Writer& writer) const {
    writer.WriteU32(height);
    writer.WriteU8(is_coinbase ? 1U : 0U);
    output.Serialize(writer);
}

bool Coin::Deserialize(Reader& reader, Coin& out, size_t max_lock_program_size) {
    Coin decoded;
    uint8_t flag = 0;
    if (!reader.ReadU32(decoded.height) || !reader.ReadU8(flag)) {
        return false;
    }
    // Exactly 0 or 1. Anything else would be a second spelling of "true", and a coin
    // with two encodings is a UTXO set two nodes can hold identically and hash
    // differently.
    if (flag > 1U) {
        reader.Fail();
        return false;
    }
    decoded.is_coinbase = flag == 1U;
    if (!TxOutput::Deserialize(reader, decoded.output, max_lock_program_size)) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

}  // namespace amarian
