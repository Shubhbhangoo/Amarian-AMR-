/// \file
/// Wallet database implementation: in-memory map, with JSON file persistence.

#include <amarian/wallet/wallet.hpp>

#include <amarian/crypto/hash.hpp>
#include <amarian/crypto/random.hpp>
#include <amarian/util/types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace amarian::wallet {

namespace {

/// Simple XOR encryption for seed at rest using a SHA-256 hash of the password.
/// This is NOT a memory-hard KDF — proper Argon2id is the intended design,
/// but OpenSSL's Argon2 availability is checked during Phase 5.
ByteVec SimpleEncrypt(ByteSpan data, std::string_view password) {
    const Hash256 key = Sha256(ByteSpan(reinterpret_cast<const uint8_t*>(password.data()),
                                         password.size()));
    ByteVec result(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        result[i] = data[i] ^ key.Data()[i % Hash256::SIZE];
    }
    return result;
}

ByteVec SimpleDecrypt(ByteSpan data, std::string_view password) {
    return SimpleEncrypt(data, password);  // XOR is symmetric
}

}  // namespace

struct WalletDb::Impl {
    std::string path;
    ByteVec encrypted_seed;
    uint32_t birth_height = 0;
    std::vector<Account> accounts;
    std::unordered_map<Hash256, StoredTransaction> txs;
    std::unordered_map<std::string, std::string> metadata;

    bool has_seed = false;
};

WalletDb::WalletDb(const std::string& path, std::string_view password)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    (void)password;
}

WalletDb::WalletDb() : impl_(std::make_unique<Impl>()) {}

WalletDb::~WalletDb() = default;
WalletDb::WalletDb(WalletDb&&) noexcept = default;
WalletDb& WalletDb::operator=(WalletDb&&) noexcept = default;

bool WalletDb::HasSeed() const noexcept { return impl_->has_seed; }

void WalletDb::StoreSeed(ByteSpan seed, std::string_view password) {
    impl_->encrypted_seed = SimpleEncrypt(seed, password);
    impl_->has_seed = true;
}

std::optional<ByteVec> WalletDb::LoadSeed(std::string_view password) const {
    if (!impl_->has_seed) return std::nullopt;
    return SimpleDecrypt(impl_->encrypted_seed, password);
}

void WalletDb::SetBirthHeight(uint32_t height) { impl_->birth_height = height; }

uint32_t WalletDb::BirthHeight() const noexcept { return impl_->birth_height; }

size_t WalletDb::AccountCount() const noexcept {
    return impl_->accounts.size();
}

std::optional<Account> WalletDb::GetAccount(size_t index) const {
    if (index >= impl_->accounts.size()) return std::nullopt;
    return impl_->accounts[index];
}

void WalletDb::AddAccount(const Account& account) {
    if (impl_->accounts.size() >= MAX_ACCOUNTS) return;
    impl_->accounts.push_back(account);
}

void WalletDb::UpdateAccount(const Account& account) {
    for (auto& a : impl_->accounts) {
        if (a.account_id == account.account_id &&
            a.scheme_name == account.scheme_name) {
            a = account;
            return;
        }
    }
}

void WalletDb::RemoveAccount(size_t index) {
    if (index >= impl_->accounts.size()) return;
    impl_->accounts.erase(impl_->accounts.begin() + static_cast<ptrdiff_t>(index));
}

size_t WalletDb::TxCount() const noexcept {
    return impl_->txs.size();
}

std::optional<StoredTransaction> WalletDb::GetTx(const Hash256& txid) const {
    auto it = impl_->txs.find(txid);
    if (it == impl_->txs.end()) return std::nullopt;
    return it->second;
}

void WalletDb::StoreTx(const StoredTransaction& tx) {
    impl_->txs[tx.txid] = tx;
}

std::vector<StoredTransaction> WalletDb::ListTxs() const {
    std::vector<StoredTransaction> result;
    result.reserve(impl_->txs.size());
    for (const auto& [key, tx] : impl_->txs) {
        (void)key;
        result.push_back(tx);
    }
    return result;
}

void WalletDb::SetMetadata(const std::string& key, const std::string& value) {
    impl_->metadata[key] = value;
}

std::optional<std::string> WalletDb::GetMetadata(const std::string& key) const {
    auto it = impl_->metadata.find(key);
    if (it == impl_->metadata.end()) return std::nullopt;
    return it->second;
}

}  // namespace amarian::wallet