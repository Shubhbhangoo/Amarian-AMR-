#pragma once

/// \file
/// Transaction construction and signing.
#include <amarian/wallet/fees.hpp>
///
/// The wallet constructs unsigned transactions from UTXOs and recipient
/// information, then signs each input with the appropriate key derived from
/// the seed. No cryptographic primitive lives here - signing is delegated to
/// libsecp256k1 and OpenSSL through the crypto layer.

#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/outpoint.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/wallet/coinselection.hpp>
#include <amarian/wallet/seed.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amarian::wallet {

/// A constructed but unsigned transaction, ready for signing.
struct UnsignedTx {
    Transaction tx;
    /// For each input, the coin being spent (contains amount and condition).
    std::vector<Coin> spent_coins;
    /// For each input, the key material to use (raw privkey bytes).
    std::vector<ByteVec> input_private_keys;
    /// The scheme used for each input (by registry id).
    std::vector<uint16_t> input_schemes;
};

/// A fully signed, broadcastable transaction.
struct SignedTx {
    Transaction tx;
    Hash256 txid;
    int64_t fee;
};

/// Builder for creating transactions from wallet state.
class TxBuilder {
public:
    TxBuilder(const MasterSeed& seed, std::string_view scheme_name,
              uint32_t account);

    /// Adds a recipient: `amount` facets to the given lock.
    void AddRecipient(const Lock& lock, int64_t amount);

    /// Sets the fee rate in facets per weight unit.
    void SetFeeRate(int64_t feerate) noexcept { fee_rate_ = feerate; }
    void SetChainId(const Hash256& chain_id) noexcept { chain_id_ = chain_id; }

    /// Sets available UTXOs from the wallet.
    void SetUtxos(std::vector<UtxoEntry> utxos) { utxos_ = std::move(utxos); }

    /// Builds an unsigned transaction.
    [[nodiscard]] std::optional<UnsignedTx> Build();

    /// Signs an unsigned transaction. Returns the signed tx.
    [[nodiscard]] std::optional<SignedTx> Sign(const UnsignedTx& utx);

private:
    MasterSeed seed_;
    std::string scheme_name_;
    uint32_t account_;
    std::vector<std::pair<Lock, int64_t>> recipients_;
    int64_t fee_rate_ = DEFAULT_FEERATE;
    Hash256 chain_id_{};
    std::vector<UtxoEntry> utxos_;

    /// Builds the SpendCondition for a single input given the key bytes and scheme.
    [[nodiscard]] SpendCondition
    BuildSpendCondition(ByteSpan privkey_bytes, uint16_t scheme_id) const;

    /// Produces a signature for one input.
    [[nodiscard]] std::optional<ByteVec>
    SignInput(const Transaction& tx, const SigHashMidstates& midstates,
              size_t input_index, int64_t spent_amount,
              const SpendCondition& condition,
              ByteSpan privkey_bytes, uint16_t scheme_id);
};

}  // namespace amarian::wallet
