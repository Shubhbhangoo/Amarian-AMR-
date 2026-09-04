#pragma once

/// \file
/// Versioned threshold-key spend-condition data.

#include <amarian/util/serialize.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace amarian {

/// A scheme-tagged public key. The crypto layer determines the meaning and
/// permitted size of a known scheme; primitives preserve unknown schemes.
struct PublicKey {
    uint16_t scheme = 0;
    ByteVec bytes;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, PublicKey& out, size_t max_key_size);

    friend bool operator==(const PublicKey&, const PublicKey&) noexcept = default;
};

/// A scheme-tagged signature, wire-identical in shape to PublicKey: a u16 scheme
/// identifier followed by a compact-size length and the signature bytes. The
/// scheme is explicit rather than inferred from length, for the same reason as a
/// key's: an unknown scheme must be preserved for relay, not guessed at parse.
struct Signature {
    uint16_t scheme = 0;
    ByteVec bytes;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool
    Deserialize(Reader& reader, Signature& out, size_t max_signature_size);

    friend bool operator==(const Signature&, const Signature&) noexcept = default;
};

/// The revealed preimage of a version-1 output lock. This is data only: ordering,
/// uniqueness, supported schemes, and threshold validity are consensus checks.
struct SpendCondition {
    uint8_t version = 0;
    uint8_t threshold = 0;
    std::vector<PublicKey> keys;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool
    Deserialize(Reader& reader, SpendCondition& out, size_t max_keys, size_t max_key_size);

    friend bool operator==(const SpendCondition&, const SpendCondition&) noexcept = default;
};

}  // namespace amarian
