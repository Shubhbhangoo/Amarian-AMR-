#pragma once

/// \file
/// Compact target encoding and the proof-of-work check.
///
/// A target is a 256-bit ceiling. The header carries it in a 32-bit
/// mantissa-and-exponent form, and two rules Bitcoin acquired by patch are rules
/// here from the start:
///
///   * **The sign bit is invalid.** The encoding reserves the mantissa's high bit
///     for a sign that a target can never legitimately have.
///   * **One encoding per target.** A non-canonical encoding is rejected, never
///     normalised. The same difficulty spelled two ways is two different headers,
///     each with a different hash — malleability in the single field that decides
///     whether work counts.
///
/// A zero target is also rejected. No hash can be at most zero, so it cannot
/// describe real work, and zero is encodable at every exponent, which is the
/// malleability the canonicality rule exists to remove.
///
/// ## Byte order, stated once
///
/// The proof-of-work comparison reads the block hash as a big-endian 256-bit
/// integer **in display order** — that is, the digest's *last* byte is the most
/// significant. Display order is the reversed digest order (see `Hash256::ToHex`),
/// so a hash that satisfies a demanding target has leading zeros when printed,
/// which is the form every block explorer and every human reader expects.
///
/// This is the same convention Bitcoin uses, and it is stated explicitly because
/// getting it backwards produces a chain that still mines, still validates, and
/// disagrees with every other implementation about which blocks are valid.

#include <amarian/util/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace amarian {

/// A 256-bit proof-of-work target, most significant byte first.
using Target = std::array<uint8_t, 32>;

/// Decodes a compact target, or nullopt if the encoding is not one a valid header
/// may carry.
///
/// Rejects: the sign bit set, a zero mantissa, a value that does not fit in 256
/// bits, a mantissa byte that would be truncated away, and any encoding that is
/// not the canonical one for the value it describes.
[[nodiscard]] std::optional<Target> CompactToTarget(uint32_t bits) noexcept;

/// The one canonical compact encoding of `target`, or 0 if `target` is zero —
/// which is not a valid encoding, so 0 is unambiguous as a failure value.
[[nodiscard]] uint32_t TargetToCompact(const Target& target) noexcept;

/// True when `hash`, read as described above, is at most `target`.
[[nodiscard]] bool HashMeetsTarget(const Hash256& hash, const Target& target) noexcept;

/// True when `bits` is a valid compact encoding *and* `hash` meets the target it
/// describes. An unusable encoding is not work, so this is one call rather than
/// two: there is no way to check the hash and forget to check the encoding.
[[nodiscard]] bool CheckProofOfWork(const Hash256& hash, uint32_t bits) noexcept;

}  // namespace amarian
