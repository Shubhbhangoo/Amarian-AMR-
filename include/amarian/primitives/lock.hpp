#pragma once

/// \file
/// Versioned output locking program.

#include <amarian/util/serialize.hpp>

#include <cstddef>
#include <cstdint>

namespace amarian {

/// A versioned output lock. This layer preserves unknown versions verbatim: the
/// decision whether a version is currently spendable is a consensus rule, while
/// relay policy decides which new locks are standard.
struct Lock {
    uint8_t version = 0;
    ByteVec program;

    void Serialize(Writer& writer) const;

    /// Decodes a bounded program. The bound is supplied by the enclosing
    /// structure because it is a resource rule, not an intrinsic property of a
    /// versioned lock. `out` is unchanged on failure.
    [[nodiscard]] static bool Deserialize(Reader& reader, Lock& out, size_t max_program_size);

    friend bool operator==(const Lock&, const Lock&) noexcept = default;
};

}  // namespace amarian
