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

template<>
struct std::hash<amarian::OutPoint> {
    /// Eight bytes of the txid mixed with the index.
    ///
    /// A txid is already a uniformly distributed 256-bit value, so eight of its bytes are
    /// as good a hash word as anything derived from them — and, unlike a generic
    /// combiner, this cannot be driven into collisions by an attacker choosing txids,
    /// because choosing a txid means finding a preimage. The index is multiplied by an
    /// odd 64-bit constant — the reciprocal of the golden ratio scaled to 2^64 — so that
    /// it influences every bit of the result rather than only the low ones, which matters
    /// because the outpoints of one transaction differ in nothing else.
    [[nodiscard]] size_t operator()(const amarian::OutPoint& outpoint) const noexcept {
        static_assert(sizeof(size_t) >= sizeof(uint64_t),
                      "the mixing below assumes a 64-bit hash word");
        constexpr size_t INDEX_MULTIPLIER = 0x9E37'79B9'7F4A'7C15ULL;
        const std::array<uint8_t, amarian::Hash256::SIZE>& bytes = outpoint.txid.Array();
        size_t mixed = 0;
        for (size_t index = 0; index < sizeof(uint64_t); ++index) {
            mixed |= size_t{bytes[index]} << (index * 8);
        }
        mixed ^= size_t{outpoint.index} * INDEX_MULTIPLIER;
        return mixed;
    }
};
