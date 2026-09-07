/// \file
/// Top-level Wallet interface implementation.

#include <amarian/wallet/wallet_api.hpp>

#include <amarian/crypto/hash.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/util/hex.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/wallet/address.hpp>
#include <amarian/wallet/signing.hpp>
#include <amarian/wallet/txbuilder.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amarian::wallet {

namespace {

constexpr std::string_view DEFAULT_HRP = "amr";
constexpr std::string_view TEST_HRP = "tamr";
constexpr uint32_t DEFAULT_ACCOUNT = 0;
constexpr std::string_view DEFAULT_SCHEME = "schnorr-secp256k1";

std::string_view HrpFor(const ChainParams& params) {
    return params.network == Network::Regtest || params.network == Network::Testnet
        ? TEST_HRP : DEFAULT_HRP;
}

}  // namespace

Wallet::Wallet(std::string_view db_path, std::string_view password,
               const ChainParams& params)
    : db_(std::string(db_path), password), params_(&params) {
    if (db_.HasSeed()) {
        if (const auto stored = db_.LoadSeed(password); stored.has_value() &&
            stored->size() == SEED_BYTES) {
            std::memcpy(seed_.data(), stored->data(), SEED_BYTES);
            return;
        }
    }
    seed_ = GenerateSeed();
    db_.StoreSeed(ByteSpan(seed_.data(), seed_.size()), password);
    db_.SetBirthHeight(0);

    Account default_acct;
    default_acct.label = "Everyday Spending";
    default_acct.scheme_name = std::string(DEFAULT_SCHEME);
    default_acct.account_id = DEFAULT_ACCOUNT;
    default_acct.highest_derived = 0;
    db_.AddAccount(default_acct);

    std::memset(next_index_by_account_, 0, sizeof(next_index_by_account_));
}

Wallet::Wallet(Wallet&&) noexcept = default;
Wallet& Wallet::operator=(Wallet&&) noexcept = default;
Wallet::~Wallet() = default;

std::optional<Wallet> Wallet::Open(std::string_view db_path,
                                   std::string_view password,
                                   const ChainParams& params) {
    Wallet wallet(db_path, password, params);
    if (!wallet.db_.HasSeed()) return std::nullopt;
    const auto seed_bytes = wallet.db_.LoadSeed(password);
    if (!seed_bytes.has_value() || seed_bytes->size() != SEED_BYTES) return std::nullopt;
    std::memcpy(wallet.seed_.data(), seed_bytes->data(), SEED_BYTES);
    return wallet;
}

Balance Wallet::GetBalance() const {
    return balance_;
}

void Wallet::Rescan(const std::vector<std::pair<Block, uint32_t>>& blocks) {
    utxos_.clear();
    balance_ = {};
    tip_height_ = blocks.empty() ? 0 : blocks.back().second;

    struct OwnedLock {
        Lock lock;
        uint32_t account = 0;
        uint32_t index = 0;
    };
    std::vector<OwnedLock> owned;
    for (size_t account_index = 0; account_index < db_.AccountCount(); ++account_index) {
        const auto account = db_.GetAccount(account_index);
        if (!account.has_value()) continue;
        for (uint32_t index = 0; index <= account->highest_derived; ++index) {
            owned.push_back({DeriveLock(account->account_id, index), account->account_id, index});
        }
    }

    for (const auto& [block, height] : blocks) {
        for (const auto& tx : block.transactions) {
            int64_t my_spent = 0;
            int64_t my_received = 0;
            bool touches_wallet = false;

            for (const auto& input : tx.inputs) {
                for (auto it = utxos_.begin(); it != utxos_.end();) {
                    if (it->outpoint == input.outpoint) {
                        touches_wallet = true;
                        my_spent += it->coin.output.amount;
                        it = utxos_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (size_t output_index = 0; output_index < tx.outputs.size(); ++output_index) {
                for (const auto& match : owned) {
                    if (tx.outputs[output_index].lock != match.lock) continue;
                    touches_wallet = true;
                    my_received += tx.outputs[output_index].amount;
                    utxos_.push_back(UtxoEntry{
                        .outpoint = OutPoint{tx.Txid(), static_cast<uint32_t>(output_index)},
                        .coin = Coin{tx.outputs[output_index], height, tx.IsCoinbase()},
                        .confirmed = true,
                        .account_id = match.account,
                        .derivation_index = match.index});
                    break;
                }
            }
            if (touches_wallet) {
                StoredTransaction stx;
                stx.txid = tx.Txid();
                stx.height = static_cast<int32_t>(height);
                stx.received = my_received;
                stx.sent = my_spent;
                stx.fee = (my_spent > my_received && !tx.IsCoinbase()) ? (my_spent - my_received) : 0;
                db_.StoreTx(stx);
            }
        }
    }

    for (const auto& entry : utxos_) {
        if (!entry.coin.is_coinbase ||
            tip_height_ >= entry.coin.height + params_->coinbase_maturity) {
            balance_.confirmed += entry.coin.output.amount;
        } else {
            // The reward is owned by this wallet, but consensus still prevents
            // spending it until coinbase maturity. Reporting it as pending keeps
            // the GUI and RPC honest without making it selectable by Send().
            balance_.pending += entry.coin.output.amount;
        }
    }
}

Lock Wallet::DeriveLock(uint32_t account_id, uint32_t index) const {
    auto acct = db_.GetAccount(account_id);
    if (!acct.has_value()) {
        return Lock{1, ByteVec(32, 0)};
    }

    std::array<uint8_t, 32> key_bytes{};
    DeriveSecp256k1Key(seed_, account_id, index,
                        MutableByteSpan(key_bytes.data(), key_bytes.size()));

    SpendCondition condition;
    condition.version = 1;
    condition.threshold = 1;
    condition.keys.resize(1);
    condition.keys[0].scheme = crypto::SCHEME_SCHNORR_SECP256K1;
    const auto keypair = GenerateSchnorrKey(ByteSpan(key_bytes.data(), key_bytes.size()));
    if (!keypair.has_value()) {
        return Lock{1, ByteVec(32, 0)};
    }
    condition.keys[0].bytes = keypair->public_key;

    const Hash256 commitment = SpendConditionCommitment(condition);

    Lock lock;
    lock.version = LOCK_VERSION_CONDITION_COMMITMENT;
    lock.program = ByteVec(commitment.Data(), commitment.Data() + Hash256::SIZE);
    return lock;
}

AddressInfo Wallet::GetNewAddress(uint32_t account_id) {
    auto acct = db_.GetAccount(account_id);
    if (!acct.has_value()) {
        return AddressInfo{};
    }

    const uint32_t index = acct->highest_derived + 1;
    Lock lock = DeriveLock(account_id, index);

    auto addr = LockToAddress(lock, HrpFor(*params_));

    acct->highest_derived = index;
    db_.UpdateAccount(*acct);

    AddressInfo info;
    info.address = addr.value_or("");
    info.lock = lock;
    info.account_id = account_id;
    info.index = index;
    return info;
}

std::vector<AddressInfo> Wallet::ListAddresses(uint32_t account_id) const {
    std::vector<AddressInfo> addresses;
    auto acct = db_.GetAccount(account_id);
    if (!acct.has_value()) return addresses;

    for (uint32_t i = 0; i <= acct->highest_derived; ++i) {
        Lock lock = DeriveLock(account_id, i);
        auto addr = LockToAddress(lock, HrpFor(*params_));
        addresses.push_back(AddressInfo{
            .address = addr.value_or(""),
            .lock = lock,
            .account_id = account_id,
            .index = i,
        });
    }
    return addresses;
}

std::optional<SendResult>
Wallet::Send(std::string_view address, int64_t amount, int64_t fee_rate) {
    auto lock = AddressToLock(address, HrpFor(*params_));
    if (!lock.has_value()) return std::nullopt;
    return SendToLock(*lock, amount, fee_rate);
}

std::optional<SendResult>
Wallet::SendToLock(const Lock& lock, int64_t amount, int64_t fee_rate) {
    TxBuilder builder(seed_, std::string(DEFAULT_SCHEME), DEFAULT_ACCOUNT);
    builder.SetChainId(params_->chain_id);
    builder.AddRecipient(lock, amount);

    std::vector<UtxoEntry> utxos;
    for (const auto& entry : utxos_) {
        if (!entry.confirmed) continue;
        if (entry.coin.is_coinbase &&
            tip_height_ < entry.coin.height + params_->coinbase_maturity) continue;
        utxos.push_back(entry);
    }
    builder.SetUtxos(utxos);

    if (fee_rate <= 0) {
        fee_rate = DEFAULT_FEERATE;
    }
    builder.SetFeeRate(fee_rate);

    if (fee_rate <= 0) {
        fee_rate = DEFAULT_FEERATE;
    }
    builder.SetFeeRate(fee_rate);

    auto utx = builder.Build();
    if (!utx.has_value()) return std::nullopt;

    auto signed_tx = builder.Sign(*utx);
    if (!signed_tx.has_value()) return std::nullopt;

    SendResult result;
    result.txid = signed_tx->txid;
    result.fee = signed_tx->fee;

    Writer writer;
    signed_tx->tx.Serialize(writer);
    result.raw_tx_hex = ToHex(writer.Take());

    StoredTransaction stx;
    stx.txid = result.txid;
    stx.height = -1;
    stx.sent = amount;
    stx.received = 0;
    stx.fee = result.fee;
    db_.StoreTx(stx);

    return result;
}

std::vector<StoredTransaction> Wallet::ListTransactions() const {
    return db_.ListTxs();
}

void Wallet::AddTransaction(const StoredTransaction& tx) {
    db_.StoreTx(tx);
}

WalletBackup Wallet::ExportBackup() const {
    return amarian::wallet::ExportBackup(seed_, db_);
}

bool Wallet::RestoreFromBackup(const WalletBackup& backup) {
    auto seed = SeedFromMnemonic(backup.mnemonic);
    if (!seed.has_value()) return false;

    seed_ = *seed;
    db_.StoreSeed(ByteSpan(seed_.data(), seed_.size()), "");

    // A mnemonic-only restore is still useful for the common default account. Full
    // backups may carry metadata, but the seed alone must not be rejected just because
    // the caller did not export that optional section.
    if (backup.metadata.empty()) {
        db_.SetBirthHeight(0);
        return true;
    }

    if (!DeserialiseMetadata(backup.metadata, db_)) {
        return false;
    }

    return true;
}

}  // namespace amarian::wallet
