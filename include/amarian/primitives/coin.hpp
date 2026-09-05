#pragma once

/// \file
/// An unspent transaction output, as the UTXO set holds it.
///
/// A `Coin` is a `TxOutput` plus the two facts about its creation that consensus needs
/// in order to decide whether it may be spent: the height of the block that created it,
/// and whether that transaction was a coinbase. Neither is derivable from the output
/// itself, and both are consensus-critical — the coinbase maturity rule is exactly the
/// conjunction of the two — so they travel with the output rather than being looked up
/// again later.
///
/// It lives in `primitives` rather than in the UTXO layer for the same reason every
/// other type here does: it has a canonical encoding, and that encoding is the one thing
/// about the UTXO set two independent nodes must agree on byte for byte if a set hash or
/// a signed snapshot is ever added. Storage decides *where* coins are kept; this decides
/// what one is.

#include <amarian/primitives/transaction.hpp>
#include <amarian/util/serialize.hpp>

#include <cstddef>
#include <cstdint>

namespace amarian {

/// An unspent output and the creation facts consensus judges it by.
struct Coin {
    /// Smallest wire form: a 4-byte height, a 1-byte coinbase flag, and the smallest
    /// output.
    static constexpr size_t MIN_SERIALIZED_SIZE =
        sizeof(uint32_t) + 1 + TxOutput::MIN_SERIALIZED_SIZE;

    TxOutput output;

    /// Height of the block whose transaction created this output.
    uint32_t height = 0;

    /// Whether the transaction that created it was a coinbase, and so whether the
    /// maturity rule applies. Stored rather than inferred: by the time an input spends
    /// this coin, the transaction that created it is not in hand.
    bool is_coinbase = false;

    /// Height and flag first, then the output.
    ///
    /// The flag is a whole byte holding 0 or 1 rather than a bit stolen from the height.
    /// Packing the two into one word saves a byte per coin and is what Bitcoin does, but
    /// it caps the representable height at 2^31 and makes overflowing that cap a silent
    /// truncation. One byte per coin is a cost worth measuring before paying for it in
    /// that way.
    void Serialize(Writer& writer) const;

    /// Leaves `out` untouched on failure. The flag byte must be exactly 0 or 1: any
    /// other value is rejected rather than treated as true, because two encodings of one
    /// coin is one encoding too many for anything that might later be hashed.
    [[nodiscard]] static bool
    Deserialize(Reader& reader, Coin& out, size_t max_lock_program_size);

    friend bool operator==(const Coin&, const Coin&) noexcept = default;
};

}  // namespace amarian
