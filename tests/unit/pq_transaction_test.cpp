/// \file
/// End-to-end post-quantum transaction tests: ML-DSA-44 signing, verification,
/// and rejection of invalid signatures.

#include <amarian/wallet/signing.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/crypto/hash.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/primitives/spend_condition.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace amarian::wallet {
namespace {

using crypto::SCHEME_ML_DSA_44;
using crypto::SCHEME_SCHNORR_SECP256K1;
using crypto::SCHEME_RESERVED;

// --- Helpers ---------------------------------------------------------------

SpendCondition MakeCondition(uint16_t scheme, ByteSpan public_key) {
    SpendCondition condition;
    condition.version = CONDITION_VERSION_THRESHOLD;
    condition.threshold = 1;
    condition.keys.resize(1);
    condition.keys[0].scheme = scheme;
    condition.keys[0].bytes = ByteVec(public_key.begin(), public_key.end());
    return condition;
}

Hash256 ComputeSighash(const Transaction& tx, size_t input_index,
                       int64_t spent_amount, const SpendCondition& condition) {
    const SigHashMidstates midstates = ComputeSigHashMidstates(tx);
    return SignatureHash(REGTEST_PARAMS.chain_id, tx, midstates,
                          static_cast<uint32_t>(input_index),
                          spent_amount, condition);
}

// --- ML-DSA-44 key generation and signing ----------------------------------

TEST(PqTransaction, Mldsa44KeyGenerationProducesCorrectSizes) {
    auto keypair = GenerateMldsa44Key();
    ASSERT_TRUE(keypair.has_value());

    const crypto::SchemeSpec* spec = crypto::FindScheme(SCHEME_ML_DSA_44);
    ASSERT_NE(spec, nullptr);

    EXPECT_EQ(keypair->public_key.size(), spec->public_key_bytes);
    EXPECT_EQ(keypair->private_key.size(), 2560); // OpenSSL raw private key is 2560 for ML-DSA-44
}

TEST(PqTransaction, Mldsa44SignAndVerify) {
    auto keypair = GenerateMldsa44Key();
    ASSERT_TRUE(keypair.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test message"), 12));
    auto signature = SignMldsa44(keypair->private_key, message);
    ASSERT_TRUE(signature.has_value());

    const crypto::SchemeSpec* spec = crypto::FindScheme(SCHEME_ML_DSA_44);
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(signature->size(), spec->signature_bytes);

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_ML_DSA_44, keypair->public_key, *signature, message);
    EXPECT_EQ(result, crypto::VerifyResult::Valid);
}

TEST(PqTransaction, Mldsa44RejectsTamperedSignature) {
    auto keypair = GenerateMldsa44Key();
    ASSERT_TRUE(keypair.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test message"), 12));
    auto signature = SignMldsa44(keypair->private_key, message);
    ASSERT_TRUE(signature.has_value());

    if (!signature->empty()) signature->front() ^= 0xFF;

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_ML_DSA_44, keypair->public_key, *signature, message);
    EXPECT_EQ(result, crypto::VerifyResult::Invalid);
}

TEST(PqTransaction, Mldsa44RejectsWrongKey) {
    auto keypair_a = GenerateMldsa44Key();
    auto keypair_b = GenerateMldsa44Key();
    ASSERT_TRUE(keypair_a.has_value());
    ASSERT_TRUE(keypair_b.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test message"), 12));
    auto signature = SignMldsa44(keypair_a->private_key, message);
    ASSERT_TRUE(signature.has_value());

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_ML_DSA_44, keypair_b->public_key, *signature, message);
    EXPECT_EQ(result, crypto::VerifyResult::Invalid);
}

TEST(PqTransaction, Mldsa44RejectsWrongMessage) {
    auto keypair = GenerateMldsa44Key();
    ASSERT_TRUE(keypair.has_value());

    const Hash256 message_a = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("message A"), 9));
    const Hash256 message_b = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("message B"), 9));
    auto signature = SignMldsa44(keypair->private_key, message_a);
    ASSERT_TRUE(signature.has_value());

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_ML_DSA_44, keypair->public_key, *signature, message_b);
    EXPECT_EQ(result, crypto::VerifyResult::Invalid);
}

TEST(PqTransaction, Mldsa44RejectsMalformedKey) {
    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test"), 4));
    ByteVec wrong_key(10, 0x42);
    ByteVec dummy_sig(2420, 0x42);

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_ML_DSA_44, wrong_key, dummy_sig, message);
    EXPECT_EQ(result, crypto::VerifyResult::Malformed);
}

TEST(PqTransaction, Mldsa44RejectsMalformedSignature) {
    auto keypair = GenerateMldsa44Key();
    ASSERT_TRUE(keypair.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test"), 4));
    ByteVec wrong_sig(10, 0x42);

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_ML_DSA_44, keypair->public_key, wrong_sig, message);
    EXPECT_EQ(result, crypto::VerifyResult::Malformed);
}

TEST(PqTransaction, Mldsa44RejectsReservedScheme) {
    auto keypair = GenerateMldsa44Key();
    ASSERT_TRUE(keypair.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test"), 4));
    auto signature = SignMldsa44(keypair->private_key, message);
    ASSERT_TRUE(signature.has_value());

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_RESERVED, keypair->public_key, *signature, message);
    EXPECT_EQ(result, crypto::VerifyResult::Reserved);
}

// --- Schnorr sign-and-verify -----------------------------------------------

TEST(PqTransaction, SchnorrKeyGeneration) {
    std::array<uint8_t, 32> seed{};
    seed.fill(0x11);
    auto keypair = GenerateSchnorrKey(seed);
    ASSERT_TRUE(keypair.has_value());
    EXPECT_EQ(keypair->public_key.size(), 32);
    EXPECT_EQ(keypair->private_key.size(), 32);
}

TEST(PqTransaction, SchnorrSignAndVerify) {
    std::array<uint8_t, 32> seed{};
    seed.fill(0x11);
    auto keypair = GenerateSchnorrKey(seed);
    ASSERT_TRUE(keypair.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test message"), 12));
    auto signature = SignSchnorr(keypair->private_key, message);
    ASSERT_TRUE(signature.has_value());
    EXPECT_EQ(signature->size(), 64);

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_SCHNORR_SECP256K1, keypair->public_key, *signature, message);
    EXPECT_EQ(result, crypto::VerifyResult::Valid);
}

TEST(PqTransaction, SchnorrRejectsTampered) {
    std::array<uint8_t, 32> seed{};
    seed.fill(0x11);
    auto keypair = GenerateSchnorrKey(seed);
    ASSERT_TRUE(keypair.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test"), 4));
    auto signature = SignSchnorr(keypair->private_key, message);
    ASSERT_TRUE(signature.has_value());
    signature->back() ^= 0xFF;

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_SCHNORR_SECP256K1, keypair->public_key, *signature, message);
    EXPECT_EQ(result, crypto::VerifyResult::Invalid);
}

// --- Cross-scheme rejection ------------------------------------------------

TEST(PqTransaction, MldsaSignatureRejectedUnderSchnorr) {
    auto mldsa_key = GenerateMldsa44Key();
    ASSERT_TRUE(mldsa_key.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test"), 4));
    auto signature = SignMldsa44(mldsa_key->private_key, message);
    ASSERT_TRUE(signature.has_value());

    std::array<uint8_t, 32> schnorr_key{};
    schnorr_key.fill(0xAA);

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_SCHNORR_SECP256K1, schnorr_key, *signature, message);
    EXPECT_EQ(result, crypto::VerifyResult::Malformed);
}

TEST(PqTransaction, SchnorrSignatureRejectedUnderMldsa) {
    std::array<uint8_t, 32> seed{};
    seed.fill(0x11);
    auto schnorr_key = GenerateSchnorrKey(seed);
    ASSERT_TRUE(schnorr_key.has_value());

    const Hash256 message = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>("test"), 4));
    auto signature = SignSchnorr(schnorr_key->private_key, message);
    ASSERT_TRUE(signature.has_value());

    auto mldsa_key = GenerateMldsa44Key();
    ASSERT_TRUE(mldsa_key.has_value());

    const crypto::VerifyResult result = crypto::Verify(
        SCHEME_ML_DSA_44, mldsa_key->public_key, *signature, message);
    EXPECT_EQ(result, crypto::VerifyResult::Malformed);
}

// --- CheckSpendAuthorisation with real ML-DSA-44 signatures ----------------

TEST(PqTransaction, CheckSpendAuthorisationWithMldsa44) {
    auto keypair = GenerateMldsa44Key();
    ASSERT_TRUE(keypair.has_value());

    SpendCondition condition = MakeCondition(SCHEME_ML_DSA_44, keypair->public_key);
    const Hash256 commitment = SpendConditionCommitment(condition);

    // Build a transaction.
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    TxInput input;
    input.outpoint = OutPoint{Hash256{}, 0};
    input.sequence = 0;
    tx.inputs.push_back(input);

    TxOutput output;
    output.amount = 100000;
    output.lock = Lock{1, ByteVec(32, 0xBB)};
    tx.outputs.push_back(output);

    // Coin the input spends.
    Coin coin;
    coin.output.amount = 100000;
    coin.output.lock = Lock{1, ByteVec(commitment.Data(), commitment.Data() + Hash256::SIZE)};
    coin.height = 1;
    coin.is_coinbase = false;

    // Sign the sighash.
    const int64_t spent_amount = 100000;
    const Hash256 sighash = ComputeSighash(tx, 0, spent_amount, condition);
    auto signature = SignMldsa44(keypair->private_key, sighash);
    ASSERT_TRUE(signature.has_value());

    // Build the witness.
    Witness witness;
    witness.condition = condition;
    Signature sig_entry;
    sig_entry.scheme = SCHEME_ML_DSA_44;
    sig_entry.bytes = std::move(*signature);
    witness.signatures.push_back(std::move(sig_entry));
    tx.witnesses.push_back(std::move(witness));

    // Verify through CheckSpendAuthorisation.
    const consensus::Verdict auth = consensus::CheckSpendAuthorisation(
        tx, std::span<const Coin>(&coin, 1), REGTEST_PARAMS);
    ASSERT_TRUE(auth.has_value()) << "spend auth failed: " << static_cast<int>(auth.error()) << " (" << consensus::Describe(auth.error()) << ")"; if (auth.has_value()) {}  // suppress unused
}

TEST(PqTransaction, CheckSpendAuthorisationWithMldsa44RejectsTampered) {
    auto keypair = GenerateMldsa44Key();
    ASSERT_TRUE(keypair.has_value());

    SpendCondition condition = MakeCondition(SCHEME_ML_DSA_44, keypair->public_key);

    const Hash256 commitment = SpendConditionCommitment(condition);
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    TxInput input;
    input.outpoint = OutPoint{Hash256{}, 0};
    input.sequence = 0;
    tx.inputs.push_back(input);

    TxOutput output;
    output.amount = 100000;
    output.lock = Lock{1, ByteVec(32, 0xBB)};
    tx.outputs.push_back(output);

    Coin coin;
    coin.output.amount = 100000;
    coin.output.lock = Lock{1, ByteVec(commitment.Data(), commitment.Data() + Hash256::SIZE)};
    coin.height = 1;
    coin.is_coinbase = false;

    const int64_t spent_amount = 100000;
    const Hash256 sighash = ComputeSighash(tx, 0, spent_amount, condition);
    auto signature = SignMldsa44(keypair->private_key, sighash);
    ASSERT_TRUE(signature.has_value());
    signature->front() ^= 0xFF;

    Witness witness;
    witness.condition = condition;
    Signature sig_entry;
    sig_entry.scheme = SCHEME_ML_DSA_44;
    sig_entry.bytes = std::move(*signature);
    witness.signatures.push_back(std::move(sig_entry));
    tx.witnesses.push_back(std::move(witness));

    const consensus::Verdict auth = consensus::CheckSpendAuthorisation(
        tx, std::span<const Coin>(&coin, 1), REGTEST_PARAMS);
    EXPECT_FALSE(auth.has_value());
    EXPECT_EQ(auth.error(), consensus::ValidationError::TxSignatureDoesNotVerify); // should fail
}

}  // namespace
}  // namespace amarian::wallet