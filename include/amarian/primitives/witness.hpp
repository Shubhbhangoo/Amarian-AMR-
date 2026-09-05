#pragma once

/// \file
/// The authorisation data revealed when an output is spent.

#include <amarian/primitives/spend_condition.hpp>

#include <cstddef>
#include <vector>

namespace amarian {

/// A spend witness: the condition that is the preimage of the output lock's
/// commitment, plus the signatures satisfying it.
///
/// Wire form: the serialised SpendCondition, then a compact-size count of
/// signatures, then that many Signature entries. Whether the count equals
/// `condition.threshold`, whether the signatures are key-ordered, and whether the
/// condition's hash matches the lock it claims to satisfy are consensus checks;
/// primitives only decode the bytes canonically and within the bounds given.
struct Witness {
    /// Smallest wire form: an empty condition and a zero signature count.
    static constexpr size_t MIN_SERIALIZED_SIZE = SpendCondition::MIN_SERIALIZED_SIZE + 1;

    SpendCondition condition;
    std::vector<Signature> signatures;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader,
                                          Witness& out,
                                          size_t max_keys,
                                          size_t max_key_size,
                                          size_t max_signatures,
                                          size_t max_signature_size);

    friend bool operator==(const Witness&, const Witness&) noexcept = default;
};

}  // namespace amarian
