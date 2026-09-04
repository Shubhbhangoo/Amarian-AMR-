#pragma once

/// \file
/// Hash constructions used by consensus.
///
/// This is the only layer that calls OpenSSL for hashing.  `Hash256` retains the
/// digest's native byte order: it is the order committed to, serialised, and
/// compared by consensus code, never its reversed display form.

#include <amarian/util/types.hpp>

#include <string_view>

namespace amarian {

/// SHA-256 of arbitrary bytes.
[[nodiscard]] Hash256 Sha256(ByteSpan bytes) noexcept;

/// SHA-256(SHA-256(bytes)). Used for transaction IDs, block IDs, and proof of
/// work. This is intentionally distinct from TaggedHash: domain separation has
/// a different construction and callers must state which one they need.
[[nodiscard]] Hash256 DoubleSha256(ByteSpan bytes) noexcept;

/// BIP-340 tagged hash: SHA256(SHA256(tag) || SHA256(tag) || message).
///
/// Tags are byte-for-byte ASCII protocol constants at call sites. The function
/// accepts string_view so a tag cannot accidentally be NUL-terminated, locale
/// transformed, or otherwise interpreted as text by the hashing layer.
[[nodiscard]] Hash256 TaggedHash(std::string_view tag, ByteSpan message);

}  // namespace amarian
