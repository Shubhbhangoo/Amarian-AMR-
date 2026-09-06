#pragma once

/// \file
/// Signing bridge for the wallet layer.
///
/// The node deliberately exposes no signing interface (amarian::crypto is
/// verification-only). The wallet needs to produce signatures for the
/// transactions it creates, so this bridge lives in the wallet layer and
/// calls OpenSSL directly, following the same pattern as the verification
/// code in src/crypto/signature.cpp.
///
/// This is the sole exception to the rule that no code outside amarian::crypto
/// calls OpenSSL's signing API. The wallet links amarian::openssl PRIVATEly
/// through amarian::crypto, and the CMake target for amarian_wallet grants
/// access to amarian::openssl for this purpose.

#include <amarian/crypto/signature.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace amarian::wallet {

/// Signs a 32-byte message with an ML-DSA-44 private key.
///
/// `private_key` must be exactly 2 562 bytes (the raw private key from
/// EVP_PKEY_new_raw_private_key_ex).
/// `message` must be exactly 32 bytes (the signature hash).
///
/// Returns the signature (2 420 bytes for ML-DSA-44) on success, or nullopt
/// if the key is malformed or the signing operation fails.
[[nodiscard]] std::optional<ByteVec>
SignMldsa44(ByteSpan private_key, const Hash256& message);

/// Signs a 32-byte message with a Schnorr (secp256k1) private key.
///
/// `private_key` must be exactly 32 bytes.
/// Returns the 64-byte Schnorr signature.
[[nodiscard]] std::optional<ByteVec>
SignSchnorr(ByteSpan private_key, const Hash256& message);

/// Generates an ML-DSA-44 key pair.
///
/// Returns {private_key, public_key} where private_key is 2 562 bytes
/// and public_key is 1 312 bytes.
struct KeyPair {
    ByteVec private_key;
    ByteVec public_key;
};
[[nodiscard]] std::optional<KeyPair> GenerateMldsa44Key();

/// Generates a Schnorr (secp256k1) key pair from a 32-byte seed.
///
/// Returns {private_key, public_key} where private_key is 32 bytes
/// and public_key is 32 bytes (x-only).
[[nodiscard]] std::optional<KeyPair> GenerateSchnorrKey(ByteSpan seed);

}  // namespace amarian::wallet