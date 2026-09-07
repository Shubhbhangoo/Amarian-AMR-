/// \file
/// Transaction construction and signing bridge.

#include <amarian/wallet/txbuilder.hpp>

#include <amarian/crypto/hash.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/primitives/spend_condition.hpp>
#include <amarian/wallet/fees.hpp>
#include <amarian/wallet/signing.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace amarian::wallet {

namespace {

/// Derives a secp256k1 private key.
void DerivePrivkey(const MasterSeed& seed, uint32_t account, uint32_t index,
                   ByteVec& privkey_out) {
    std::array<uint8_t, 32> key{};
    DeriveSecp256k1Key(seed, account, index, MutableByteSpan(key));
    privkey_out.assign(key.begin(), key.end());
}

/// Returns the scheme id from the scheme name.
uint16_t SchemeIdFromName(std::string_view name) {
    if (name == "schnorr-secp256k1") return crypto::SCHEME_SCHNORR_SECP256K1;
    if (name == "ml-dsa-44") return crypto::SCHEME_ML_DSA_44;
    if (name == "slh-dsa-sha2-128s") return crypto::SCHEME_SLH_DSA_SHA2_128S;
    return 0;
}

}  // namespace

TxBuilder::TxBuilder(const MasterSeed& seed, std::string_view scheme_name,
                     uint32_t account)
    : seed_(seed), scheme_name_(scheme_name), account_(account) {}

void TxBuilder::AddRecipient(const Lock& lock, int64_t amount) {
    recipients_.emplace_back(lock, amount);
}

std::optional<UnsignedTx> TxBuilder::Build() {
    if (recipients_.empty() || utxos_.empty()) return std::nullopt;

    // Compute total target from recipients.
    int64_t target = 0;
    for (const auto& [lock, amount] : recipients_) {
        (void)lock;
        target += amount;
    }

    // Select coins.
    CoinSelection selection = SelectCoins(utxos_, target, fee_rate_);
    if (!selection.success) return std::nullopt;

    UnsignedTx utx;
    utx.tx.version = 1;
    utx.tx.locktime = 0;

    for (const auto& entry : selection.inputs) {
        TxInput input;
        input.outpoint = entry.outpoint;
        input.sequence = 0;
        utx.tx.inputs.push_back(input);
        utx.spent_coins.push_back(entry.coin);
        utx.input_schemes.push_back(SchemeIdFromName(scheme_name_));

        // Derive key for this input.
        ByteVec privkey(32);
        DerivePrivkey(seed_, entry.account_id == 0 ? account_ : entry.account_id,
                      entry.derivation_index, privkey);
        utx.input_private_keys.push_back(std::move(privkey));
    }

    // Add recipient outputs.
    for (const auto& [lock, amount] : recipients_) {
        TxOutput output;
        output.amount = amount;
        output.lock = lock;
        utx.tx.outputs.push_back(output);
    }

    // Add change output if needed.
    if (selection.change > 0 && selection.change >= DUST_THRESHOLD) {
        TxOutput change_out;
        change_out.amount = selection.change;
        // Use the same lock pattern as the first input's coin.
        if (!utxos_.empty()) {
            change_out.lock = utxos_[0].coin.output.lock;
        } else {
            change_out.lock = Lock{1, ByteVec(32, 0)};
        }
        utx.tx.outputs.push_back(change_out);
    }

    return utx;
}

SpendCondition TxBuilder::BuildSpendCondition(ByteSpan privkey_bytes,
                                              uint16_t scheme_id) const {
    SpendCondition condition;
    condition.version = 1;
    condition.threshold = 1;
    condition.keys.resize(1);
    condition.keys[0].scheme = scheme_id;
    if (scheme_id == crypto::SCHEME_SCHNORR_SECP256K1) {
        const auto keypair = GenerateSchnorrKey(privkey_bytes);
        if (keypair.has_value()) condition.keys[0].bytes = keypair->public_key;
    } else {
        condition.keys[0].bytes = ByteVec(privkey_bytes.begin(), privkey_bytes.end());
    }
    return condition;
}

std::optional<ByteVec>
TxBuilder::SignInput(const Transaction& tx, const SigHashMidstates& midstates,
                     size_t input_index, int64_t spent_amount,
                     const SpendCondition& condition,
                     ByteSpan privkey_bytes, uint16_t scheme_id) {
    // Compute the signature hash.
    const Hash256 sighash = SignatureHash(chain_id_, tx, midstates,
                                          static_cast<uint32_t>(input_index),
                                          spent_amount, condition);

    if (scheme_id == crypto::SCHEME_SCHNORR_SECP256K1) {
        return SignSchnorr(privkey_bytes, sighash);
    }
    if (scheme_id == crypto::SCHEME_ML_DSA_44) {
        return SignMldsa44(privkey_bytes, sighash);
    }
    return std::nullopt;
}

std::optional<SignedTx> TxBuilder::Sign(const UnsignedTx& utx) {
    if (utx.tx.inputs.size() != utx.spent_coins.size()) return std::nullopt;
    if (utx.tx.inputs.size() != utx.input_private_keys.size()) return std::nullopt;

    Transaction signed_tx = utx.tx;
    const SigHashMidstates midstates = ComputeSigHashMidstates(signed_tx);

    for (size_t i = 0; i < signed_tx.inputs.size(); ++i) {
        SpendCondition condition =
            BuildSpendCondition(utx.input_private_keys[i], utx.input_schemes[i]);

        auto sig = SignInput(signed_tx, midstates, i,
                             utx.spent_coins[i].output.amount,
                             condition,
                             utx.input_private_keys[i],
                             utx.input_schemes[i]);
        if (!sig.has_value()) return std::nullopt;

        Witness witness;
        witness.condition = condition;
        Signature sig_entry;
        sig_entry.scheme = utx.input_schemes[i];
        sig_entry.bytes = std::move(*sig);
        witness.signatures.push_back(std::move(sig_entry));
        signed_tx.witnesses.push_back(std::move(witness));
    }

    int64_t input_total = 0;
    for (const auto& coin : utx.spent_coins) {
        input_total += coin.output.amount;
    }
    int64_t output_total = 0;
    for (const auto& output : signed_tx.outputs) {
        output_total += output.amount;
    }

    SignedTx result;
    result.tx = std::move(signed_tx);
    result.txid = result.tx.Txid();
    result.fee = input_total - output_total;

    return result;
}

}  // namespace amarian::wallet
