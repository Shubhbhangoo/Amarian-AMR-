/// \file
/// Hybrid ownership: 2-of-2 SpendCondition with one Schnorr key and one ML-DSA-44 key.
///
/// The consensus threshold evaluator already supports this natively: a
/// SpendCondition with threshold=2 and two keys (one Schnorr, one ML-DSA-44)
/// is evaluated by the ordered-forward-match algorithm, which requires one
/// signature of each scheme. No new consensus code is needed.
///
/// This file provides the wallet-side helpers to build and sign hybrid
/// transactions.

#include <amarian/crypto/signature.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/primitives/spend_condition.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/wallet/seed.hpp>
#include <amarian/wallet/signing.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace amarian::wallet {

/// Builds a 2-of-2 hybrid SpendCondition: one Schnorr key, one ML-DSA-44 key.
///
/// Keys must be sorted ascending by the consensus rule (PublicKey::operator<=>).
/// Schnorr scheme id (1) < ML-DSA-44 scheme id (2), so the Schnorr key comes first.
[[nodiscard]] inline SpendCondition
MakeHybridCondition(ByteSpan schnorr_pubkey, ByteSpan mldsa44_pubkey) {
    SpendCondition condition;
    condition.version = CONDITION_VERSION_THRESHOLD;
    condition.threshold = 2;
    condition.keys.resize(2);

    // Key 0: Schnorr (scheme 1, sorts before ML-DSA-44's scheme 2).
    condition.keys[0].scheme = crypto::SCHEME_SCHNORR_SECP256K1;
    condition.keys[0].bytes = ByteVec(schnorr_pubkey.begin(), schnorr_pubkey.end());

    // Key 1: ML-DSA-44 (scheme 2, sorts after Schnorr).
    condition.keys[1].scheme = crypto::SCHEME_ML_DSA_44;
    condition.keys[1].bytes = ByteVec(mldsa44_pubkey.begin(), mldsa44_pubkey.end());

    return condition;
}

/// Signs a transaction input with both a Schnorr and an ML-DSA-44 signature,
/// producing a hybrid witness.
///
/// `spent_coin` is the coin being spent (must be locked to a hybrid commitment).
/// Returns the fully witnessed transaction, or nullopt if signing fails.
[[nodiscard]] inline std::optional<Transaction>
SignHybridInput(const Transaction& tx, const Coin& spent_coin,
                size_t input_index, const SpendCondition& condition,
                ByteSpan schnorr_privkey, ByteSpan mldsa44_privkey,
                const ChainParams& params) {
    if (input_index >= tx.inputs.size()) return std::nullopt;
    if (input_index >= tx.witnesses.size()) return std::nullopt;

    // Compute the signature hash.
    const SigHashMidstates midstates = ComputeSigHashMidstates(tx);
    const Hash256 sighash = SignatureHash(
        params.chain_id, tx, midstates,
        static_cast<uint32_t>(input_index),
        spent_coin.output.amount, condition);

    // Sign with Schnorr (key 0 in the sorted condition).
    auto schnorr_sig = SignSchnorr(schnorr_privkey, sighash);
    if (!schnorr_sig.has_value()) return std::nullopt;

    // Sign with ML-DSA-44 (key 1 in the sorted condition).
    auto mldsa_sig = SignMldsa44(mldsa44_privkey, sighash);
    if (!mldsa_sig.has_value()) return std::nullopt;

    // Build the hybrid witness: exactly 2 signatures, in key order (Schnorr first).
    Transaction signed_tx = tx;
    Witness& witness = signed_tx.witnesses[input_index];
    witness.condition = condition;
    witness.signatures.clear();
    witness.signatures.resize(2);

    witness.signatures[0].scheme = crypto::SCHEME_SCHNORR_SECP256K1;
    witness.signatures[0].bytes = std::move(*schnorr_sig);

    witness.signatures[1].scheme = crypto::SCHEME_ML_DSA_44;
    witness.signatures[1].bytes = std::move(*mldsa_sig);

    return signed_tx;
}

/// Verifies a hybrid transaction through the full consensus
/// CheckSpendAuthorisation path.
[[nodiscard]] inline bool
VerifyHybridTransaction(const Transaction& tx, std::span<const Coin> spent_coins,
                        const ChainParams& params) {
    const consensus::Verdict auth = consensus::CheckSpendAuthorisation(
        tx, spent_coins, params);
    return auth.has_value();
}

}  // namespace amarian::wallet