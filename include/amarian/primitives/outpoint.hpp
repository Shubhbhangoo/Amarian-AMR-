#pragma once

/// \file
/// Canonical reference to one transaction output.

#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>

#include <cstdint>

namespace amarian {

/// Identifies output `index` of the transaction whose internal-order ID is
/// `txid`. It has no special values at this layer: the coinbase sentinel is a
/// transaction structural rule, not a different wire type.
struct OutPoint {
    /// Fixed on the wire: a 32-byte txid and a 4-byte index.
    static constexpr size_t SERIALIZED_SIZE = Hash256::SIZE + sizeof(uint32_t);

    Hash256 txid{};
    uint32_t index = 0;

    void Serialize(Writer& writer) const;

    /// Leaves `out` untouched on failure, matching Reader's whole-field
    /// semantics. Semantic validity (for example whether it is unspent) belongs
    /// to the UTXO and consensus layers.
    [[nodiscard]] static bool Deserialize(Reader& reader, OutPoint& out) noexcept;

    friend constexpr bool operator==(const OutPoint&, const OutPoint&) noexcept = default;
};

}  // namespace amarian
