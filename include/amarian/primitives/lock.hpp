#pragma once

/// \file
/// Versioned output locking program.

#include <amarian/util/serialize.hpp>

#include <cstddef>
#include <cstdint>

namespace amarian {

/// A lock that can never be satisfied: there is no spend condition to commit to, so
/// no witness exists that consensus would accept.
///
/// Version 0 is carved out of the "unknown version is spendable" rule deliberately,
/// and in the safe direction. Unknown versions must be spendable so that a new scheme
/// can be deployed by soft fork without splitting old nodes off the chain; the cost
/// is that an unknown version is, to an old node, anyone-can-spend. Reserving one
/// version as permanently unspendable gives a provable burn — used by genesis, whose
/// zero-value output must be unspendable as a rule rather than by improbability — and
/// costs nothing, because it is fixed before any block exists. It can never be
/// relaxed later: making version 0 spendable would be a hard fork, which is precisely
/// what "burned" has to mean.
///
/// It also makes the default-constructed `Lock` unspendable rather than
/// anyone-can-spend, so a lock a caller forgot to fill in loses the coins instead of
/// giving them away.
inline constexpr uint8_t LOCK_VERSION_UNSPENDABLE = 0;

/// The commitment lock: `program` is
/// `TaggedHash("Amarian/SpendCondition", serialise(SpendCondition))`.
inline constexpr uint8_t LOCK_VERSION_CONDITION_COMMITMENT = 1;

/// A versioned output lock. This layer preserves unknown versions verbatim: the
/// decision whether a version is currently spendable is a consensus rule, while
/// relay policy decides which new locks are standard.
struct Lock {
    uint8_t version = LOCK_VERSION_UNSPENDABLE;
    ByteVec program;

    /// Whether no witness can ever satisfy this lock. Consensus rejects any input
    /// spending such an output, and a UTXO set may drop it rather than store it.
    [[nodiscard]] constexpr bool IsUnspendable() const noexcept {
        return version == LOCK_VERSION_UNSPENDABLE;
    }

    void Serialize(Writer& writer) const;

    /// Decodes a bounded program. The bound is supplied by the enclosing
    /// structure because it is a resource rule, not an intrinsic property of a
    /// versioned lock. `out` is unchanged on failure.
    [[nodiscard]] static bool Deserialize(Reader& reader, Lock& out, size_t max_program_size);

    friend bool operator==(const Lock&, const Lock&) noexcept = default;
};

}  // namespace amarian
