/// \file
/// The scheme registry against real signatures.
///
/// Schnorr is checked against BIP-340's published vector 0, which is the only kind of
/// check worth having for a classical scheme: agreement with the specification, not
/// with this build.
///
/// The two NIST schemes have no vector file in the tree, so the test generates a key
/// pair with OpenSSL, signs, and verifies through Amarian's registry. That is why this
/// target — and only this target — links OpenSSL directly: the node never signs, so
/// the library deliberately exposes no signing interface, and the alternative to
/// generating a key here is leaving the post-quantum verification path with no test at
/// all. The sizes the table claims are then measured rather than asserted, so an
/// OpenSSL upgrade that changed one fails here instead of on a live chain.

#include <amarian/crypto/signature.hpp>

#include <openssl/core_names.h>
#include <openssl/evp.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace amarian::crypto {
namespace {

// BIP-340 test vector 0.
constexpr std::array<uint8_t, 32> BIP340_PUBKEY{
    0xF9, 0x30, 0x8A, 0x01, 0x92, 0x58, 0xC3, 0x10, 0x49, 0x34, 0x4F, 0x85, 0xF8, 0x9D, 0x52, 0x29,
    0xB5, 0x31, 0xC8, 0x45, 0x83, 0x6F, 0x99, 0xB0, 0x86, 0x01, 0xF1, 0x13, 0xBC, 0xE0, 0x36, 0xF9};
constexpr std::array<uint8_t, 64> BIP340_SIGNATURE{
    0xE9, 0x07, 0x83, 0x1F, 0x80, 0x84, 0x8D, 0x10, 0x69, 0xA5, 0x37, 0x1B, 0x40, 0x24, 0x10, 0x36,
    0x4B, 0xDF, 0x1C, 0x5F, 0x83, 0x07, 0xB0, 0x08, 0x4C, 0x55, 0xF1, 0xCE, 0x2D, 0xCA, 0x82, 0x15,
    0x25, 0xF6, 0x6A, 0x4A, 0x85, 0xEA, 0x8B, 0x71, 0xE4, 0x82, 0xA7, 0x4F, 0x38, 0x2D, 0x2C, 0xE5,
    0xEB, 0xEE, 0xE8, 0xFD, 0xB2, 0x17, 0x2F, 0x47, 0x7D, 0xF4, 0x90, 0x0D, 0x31, 0x05, 0x36, 0xC0};
// Vector 0's message is 32 zero bytes, which is Hash256's default.
const Hash256 BIP340_MESSAGE{};

/// A key pair and one signature over `message`, produced by OpenSSL.
struct PqSample {
    ByteVec public_key;
    ByteVec signature;
};

/// Generates a key, exports its raw public bytes, and signs `message`.
///
/// Uses the same one-shot `EVP_PKEY_sign_message_init` path the registry verifies
/// with, so a mismatch between how OpenSSL signs and how Amarian asks it to verify
/// would show up here.
[[nodiscard]] PqSample MakePqSample(const char* algorithm, const Hash256& message) {
    PqSample sample;

    EVP_PKEY_CTX* gen = EVP_PKEY_CTX_new_from_name(nullptr, algorithm, nullptr);
    EVP_PKEY* key = nullptr;
    if (gen == nullptr || EVP_PKEY_keygen_init(gen) <= 0 || EVP_PKEY_generate(gen, &key) <= 0) {
        EVP_PKEY_CTX_free(gen);
        return sample;
    }
    EVP_PKEY_CTX_free(gen);

    size_t public_len = 0;
    EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &public_len);
    sample.public_key.resize(public_len);
    EVP_PKEY_get_octet_string_param(
        key, OSSL_PKEY_PARAM_PUB_KEY, sample.public_key.data(), public_len, &public_len);
    sample.public_key.resize(public_len);

    EVP_SIGNATURE* alg = EVP_SIGNATURE_fetch(nullptr, algorithm, nullptr);
    EVP_PKEY_CTX* sign_ctx = EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr);
    size_t signature_len = 0;
    if (alg != nullptr && sign_ctx != nullptr &&
        EVP_PKEY_sign_message_init(sign_ctx, alg, nullptr) > 0 &&
        EVP_PKEY_sign(sign_ctx, nullptr, &signature_len, message.Data(), Hash256::SIZE) > 0) {
        sample.signature.resize(signature_len);
        if (EVP_PKEY_sign(sign_ctx,
                          sample.signature.data(),
                          &signature_len,
                          message.Data(),
                          Hash256::SIZE) > 0) {
            sample.signature.resize(signature_len);
        } else {
            sample.signature.clear();
        }
    }

    EVP_PKEY_CTX_free(sign_ctx);
    EVP_SIGNATURE_free(alg);
    EVP_PKEY_free(key);
    return sample;
}

/// Signs, verifies, mutates, re-verifies. The whole point of a scheme being in the
/// table is that both halves of this come out right.
void ExpectRoundTrip(uint16_t scheme, const char* openssl_name) {
    const SchemeSpec* spec = FindScheme(scheme);
    ASSERT_NE(spec, nullptr);

    const Hash256 message{};
    const PqSample sample = MakePqSample(openssl_name, message);
    ASSERT_EQ(sample.public_key.size(), spec->public_key_bytes)
        << openssl_name << ": the table's public key size is not the one OpenSSL produces";
    ASSERT_EQ(sample.signature.size(), spec->signature_bytes)
        << openssl_name << ": the table's signature size is not the one OpenSSL produces";

    EXPECT_EQ(Verify(scheme, sample.public_key, sample.signature, message), VerifyResult::Valid);

    ByteVec mutated = sample.signature;
    mutated[0] = static_cast<uint8_t>(mutated[0] ^ 1U);
    EXPECT_EQ(Verify(scheme, sample.public_key, mutated, message), VerifyResult::Invalid);

    // A different message under the same key and signature must also fail: this is what
    // makes the signature a claim about the sighash rather than about the key.
    Hash256 other_message{};
    other_message.Array()[0] = 1;
    EXPECT_EQ(Verify(scheme, sample.public_key, sample.signature, other_message),
              VerifyResult::Invalid);
}

TEST(CryptoSignature, SchnorrVerifiesBip340Vector) {
    EXPECT_EQ(Verify(SCHEME_SCHNORR_SECP256K1, BIP340_PUBKEY, BIP340_SIGNATURE, BIP340_MESSAGE),
              VerifyResult::Valid);
}

TEST(CryptoSignature, SchnorrRejectsAMutatedSignature) {
    auto mutated = BIP340_SIGNATURE;
    mutated[0] = static_cast<uint8_t>(mutated[0] ^ 1U);
    EXPECT_EQ(Verify(SCHEME_SCHNORR_SECP256K1, BIP340_PUBKEY, mutated, BIP340_MESSAGE),
              VerifyResult::Invalid);
}

TEST(CryptoSignature, SchnorrSeparatesAnUnusableKeyFromAFailedVerification) {
    // All-zero is not the x coordinate of a curve point, so nothing is verified and the
    // answer is Malformed rather than Invalid.
    const std::array<uint8_t, 32> not_a_point{};
    EXPECT_EQ(Verify(SCHEME_SCHNORR_SECP256K1, not_a_point, BIP340_SIGNATURE, BIP340_MESSAGE),
              VerifyResult::Malformed);
}

TEST(CryptoSignature, ReservedSchemeIsNeverVerifiable) {
    EXPECT_EQ(Verify(SCHEME_RESERVED, BIP340_PUBKEY, BIP340_SIGNATURE, BIP340_MESSAGE),
              VerifyResult::Reserved);
    EXPECT_EQ(FindScheme(SCHEME_RESERVED), nullptr);
}

TEST(CryptoSignature, UnknownSchemeIsDistinctFromInvalid) {
    // The soft-fork path: a scheme this build has never heard of must be reported as
    // unknown so consensus can keep the output spendable, not as a forgery.
    EXPECT_EQ(Verify(9'999, BIP340_PUBKEY, BIP340_SIGNATURE, BIP340_MESSAGE),
              VerifyResult::UnknownScheme);
    EXPECT_EQ(FindScheme(9'999), nullptr);
}

TEST(CryptoSignature, WrongLengthsAreRejectedBeforeAnyImplementationIsCalled) {
    const std::array<uint8_t, 31> short_key{};
    const std::array<uint8_t, 65> long_signature{};
    EXPECT_EQ(Verify(SCHEME_SCHNORR_SECP256K1, short_key, BIP340_SIGNATURE, BIP340_MESSAGE),
              VerifyResult::Malformed);
    EXPECT_EQ(Verify(SCHEME_SCHNORR_SECP256K1, BIP340_PUBKEY, long_signature, BIP340_MESSAGE),
              VerifyResult::Malformed);
    EXPECT_FALSE(HasWellFormedSizes(SCHEME_SCHNORR_SECP256K1, 31, 64));
    EXPECT_TRUE(HasWellFormedSizes(SCHEME_SCHNORR_SECP256K1, 32, 64));
    EXPECT_FALSE(HasWellFormedSizes(SCHEME_RESERVED, 32, 64));
}

TEST(CryptoSignature, EverySchemeInTheTableIsAvailableInThisBuild) {
    // The startup check a node makes. If this fails, the build's OpenSSL cannot provide
    // an algorithm the table claims, and the node must refuse to run rather than accept
    // signatures nothing checked.
    EXPECT_EQ(FirstUnavailableScheme(), SCHEME_RESERVED);
}

TEST(CryptoSignature, TheTableIsDenseAscendingAndSelfConsistent) {
    const auto schemes = KnownSchemes();
    ASSERT_FALSE(schemes.empty());
    for (size_t i = 0; i < schemes.size(); ++i) {
        EXPECT_EQ(schemes[i].id, uint16_t{static_cast<uint16_t>(i + 1)});
        EXPECT_EQ(FindScheme(schemes[i].id), &schemes[i]);
        EXPECT_FALSE(schemes[i].name.empty());
        EXPECT_GT(schemes[i].public_key_bytes, 0U);
        EXPECT_GT(schemes[i].signature_bytes, 0U);
    }
}

TEST(CryptoSignature, MlDsa44RoundTrips) {
    ExpectRoundTrip(SCHEME_ML_DSA_44, "ML-DSA-44");
}

TEST(CryptoSignature, SlhDsaSha2128sRoundTrips) {
    ExpectRoundTrip(SCHEME_SLH_DSA_SHA2_128S, "SLH-DSA-SHA2-128s");
}

TEST(CryptoSignature, ThePostQuantumSchemesRestOnUnrelatedAssumptions) {
    // Not a cryptographic check — a check that the table still offers a choice. If both
    // post-quantum rows ever named the same backend family and the same hardness
    // assumption, the second one would no longer be a hedge against the first.
    const SchemeSpec* lattice = FindScheme(SCHEME_ML_DSA_44);
    const SchemeSpec* hash_based = FindScheme(SCHEME_SLH_DSA_SHA2_128S);
    ASSERT_NE(lattice, nullptr);
    ASSERT_NE(hash_based, nullptr);
    EXPECT_EQ(lattice->scheme_class, SchemeClass::PostQuantum);
    EXPECT_EQ(hash_based->scheme_class, SchemeClass::PostQuantum);
    EXPECT_NE(lattice->name, hash_based->name);

    const SchemeSpec* classical = FindScheme(SCHEME_SCHNORR_SECP256K1);
    ASSERT_NE(classical, nullptr);
    EXPECT_EQ(classical->scheme_class, SchemeClass::Classical);
}

}  // namespace
}  // namespace amarian::crypto
