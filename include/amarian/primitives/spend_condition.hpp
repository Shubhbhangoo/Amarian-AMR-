#pragma once

/// \file
/// Versioned threshold-key spend-condition data.

#include <amarian/util/serialize.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace amarian {

/// The scheme identifier that is never valid.
///
/// Reserved so that an all-zero `scheme` field is not a usable scheme. An unknown
/// scheme has to be preserved on the wire and left for a future soft fork to define,
/// which means "unknown" cannot double as "absent" — a zeroed or truncated-then-padded
/// field would otherwise name a real scheme. Nothing may verify under it.
inline constexpr uint16_t SCHEME_RESERVED = 0;

/// The condition version that is never valid, for the same reason as `SCHEME_RESERVED`:
/// a default-constructed or zero-filled `SpendCondition` must not be satisfiable.
inline constexpr uint8_t CONDITION_VERSION_RESERVED = 0;

/// Threshold over a list of scheme-tagged keys: `threshold` signatures from distinct
/// keys in `keys`. The only condition version Phase 1 defines, and the one all
/// single-key, multisignature and hybrid ownership is expressed in.
inline constexpr uint8_t CONDITION_VERSION_THRESHOLD = 1;

/// A scheme-tagged public key. The crypto layer determines the meaning and
/// permitted size of a known scheme; primitives preserve unknown schemes.
struct PublicKey {
    /// Smallest wire form: a 2-byte scheme and a compact-size zero length. Used to
    /// bound a key count against the bytes actually remaining, before any
    /// allocation.
    static constexpr size_t MIN_SERIALIZED_SIZE = 3;

    uint16_t scheme = 0;
    ByteVec bytes;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, PublicKey& out, size_t max_key_size);

    friend bool operator==(const PublicKey&, const PublicKey&) noexcept = default;

    /// Ascending by `scheme`, then lexicographically by `bytes`.
    ///
    /// This is the order the "keys are sorted and distinct" consensus rule is stated
    /// in, and it exists here, once, so that the rule and any code that produces a
    /// condition cannot disagree about what sorted means. Since every known scheme has
    /// one key length, within a scheme this is plain byte order.
    friend std::strong_ordering operator<=>(const PublicKey&, const PublicKey&) noexcept = default;
};

/// A scheme-tagged signature, wire-identical in shape to PublicKey: a u16 scheme
/// identifier followed by a compact-size length and the signature bytes. The
/// scheme is explicit rather than inferred from length, for the same reason as a
/// key's: an unknown scheme must be preserved for relay, not guessed at parse.
struct Signature {
    /// Same shape as a key, so the same floor: 2-byte scheme, zero length.
    static constexpr size_t MIN_SERIALIZED_SIZE = 3;

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
    /// Smallest wire form: version, threshold, and a compact-size zero key count.
    static constexpr size_t MIN_SERIALIZED_SIZE = 3;

    uint8_t version = 0;
    uint8_t threshold = 0;
    std::vector<PublicKey> keys;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool
    Deserialize(Reader& reader, SpendCondition& out, size_t max_keys, size_t max_key_size);

    friend bool operator==(const SpendCondition&, const SpendCondition&) noexcept = default;
};

}  // namespace amarian
