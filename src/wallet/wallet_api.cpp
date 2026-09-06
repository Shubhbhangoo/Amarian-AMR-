/// \file
/// Top-level Wallet interface implementation.

#include <amarian/wallet/wallet_api.hpp>

#include <amarian/crypto/hash.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/wallet/address.hpp>
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
    WalletDb db(std::string(db_path), password);
    if (!db.HasSeed()) return std::nullopt;

    auto seed_bytes = db.LoadSeed(password);
    if (!seed_bytes.has_value() || seed_bytes->size() != SEED_BYTES) {
        return std::nullopt;
    }

    Wallet wallet(db_path, password, params);
    std::memcpy(wallet.seed_.data(), seed_bytes->data(), SEED_BYTES);
    return wallet;
}

Balance Wallet::GetBalance() const {
    return balance_;
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
    condition.keys[0].bytes = ByteVec(key_bytes.begin(), key_bytes.end());

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
    builder.AddRecipient(lock, amount);

    std::vector<UtxoEntry> utxos;
    builder.SetUtxos(utxos);

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

    if (!DeserialiseMetadata(backup.metadata, db_)) {
        return false;
    }

    return true;
}

}  // namespace amarian::wallet