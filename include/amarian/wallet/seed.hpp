#pragma once

/// \file
/// Key derivation from a master seed using HKDF-SHA256.
///
/// The wallet holds a single 256-bit master seed (from a recovery phrase or
/// generated at creation). Per-scheme child key material is derived via
///
///     HKDF-SHA256(
///         salt = "Amarian/KeyDeriv/v1" || scheme_name,
///         ikm  = master_seed,
///         info = account || index
///     )
///
/// The output is exactly the number of bytes the scheme's keygen expects - 32 for
/// Schnorr, 32 for ML-DSA-44, and the scheme-defined seed lengths for SLH-DSA.
///
/// There is no public derivation. BIP-32's arithmetic on secp256k1 scalars does
/// not generalise to lattice or hash-based schemes, so every key requires the seed.
/// A wallet can export an explicit list of addresses for watch-only use.

#include <amarian/crypto/hash.hpp>
#include <amarian/util/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace amarian::wallet {

/// The fixed size of the master seed.
inline constexpr size_t SEED_BYTES = 32;

/// A master seed, from which every wallet key is derived.
using MasterSeed = std::array<uint8_t, SEED_BYTES>;

/// Maximum account number the derivation supports.
inline constexpr uint32_t MAX_ACCOUNT = 0xFFFFU;

/// Maximum index per account (address gap limit).
inline constexpr uint32_t MAX_INDEX = 0xFFFFU;

/// Derives key material for `scheme_name`, `account`, `index` from the master seed.
///
/// `scheme_name` is domain-separated: the same (account, index) under two
/// different schemes produces unrelated keys, so a break in one scheme cannot
/// leak information about keys under another.
///
/// `out_bytes` determines how many bytes are derived. The caller knows the
/// scheme's seed length from the registry.
void DeriveKeyBytes(const MasterSeed& seed, std::string_view scheme_name,
                    uint32_t account, uint32_t index, MutableByteSpan out_bytes);

/// Derives a 32-byte secp256k1 private key (reduced into range) from the seed.
///
/// Returns the 32 bytes that libsecp256k1 expects as a private key.
void DeriveSecp256k1Key(const MasterSeed& seed, uint32_t account, uint32_t index,
                        MutableByteSpan out_key);

/// Derives a 32-byte ML-DSA-44 seed from the master seed.
///
/// ML-DSA-44 keygen takes exactly 32 bytes as its seed (FIPS 204 section 5.1).
void DeriveMldsa44Seed(const MasterSeed& seed, uint32_t account, uint32_t index,
                       MutableByteSpan out_seed);

/// Derives the three n-byte seeds SLH-DSA-SHA2-128s keygen expects.
///
/// SLH-DSA-SHA2-128s takes three 16-byte seeds: sk_seed, sk_prf, pk_seed.
void DeriveSlhDsaSeeds(const MasterSeed& seed, uint32_t account, uint32_t index,
                       MutableByteSpan sk_seed, MutableByteSpan sk_prf,
                       MutableByteSpan pk_seed);

/// Generates a fresh random master seed from the OS CSPRNG.
[[nodiscard]] MasterSeed GenerateSeed();

}  // namespace amarian::wallet
