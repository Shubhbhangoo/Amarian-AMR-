#pragma once

/// \file
/// Cryptographically strong random bytes.
///
/// The second thing this layer exists to be the only place for, after hashing: a node needs
/// unpredictable bytes for an RPC cookie today and for wallet key material later, and both
/// must come from the operating system's entropy source rather than from a pseudo-random
/// generator seeded by a clock. `std::random_device` is not usable for this — the standard
/// permits it to be a deterministic sequence, and at least one shipping implementation makes
/// it one — so this is OpenSSL's `RAND_bytes`, which draws from the platform's CSPRNG.
///
/// Isolated here for the same reason hashing is: `amarian::crypto` is the sole OpenSSL
/// boundary, so a layer that needs randomness asks for randomness rather than acquiring the
/// ability to call anything else OpenSSL offers.

#include <amarian/util/types.hpp>

#include <cstdint>
#include <span>

namespace amarian::crypto {

/// Fills `out` with cryptographically strong random bytes, returning whether it managed to.
///
/// False means the platform's entropy source failed, which is not a condition a caller may
/// paper over: there is no weaker fallback here on purpose, because a fallback is exactly how
/// a key or a token ends up predictable. A caller that cannot proceed without randomness must
/// fail, and one that can must say so at its own call site.
[[nodiscard]] bool RandomBytes(std::span<uint8_t> out) noexcept;

/// `count` random bytes, or an empty vector if the entropy source failed.
///
/// The empty vector is unambiguous because a caller asking for zero bytes has asked for
/// nothing and cannot be harmed by getting it.
[[nodiscard]] ByteVec RandomByteVec(size_t count);

}  // namespace amarian::crypto
