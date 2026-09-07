/// \file
/// Wallet database implementation: in-memory map, with JSON file persistence.

#include <amarian/wallet/wallet.hpp>

#include <amarian/crypto/hash.hpp>
#include <amarian/crypto/random.hpp>
#include <amarian/util/hex.hpp>
#include <amarian/util/types.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace amarian::wallet {

namespace {

using json = nlohmann::json;

/// Simple XOR encryption for seed at rest using a SHA-256 hash of the password.
/// This is NOT a memory-hard KDF - proper Argon2id is the intended design,
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

    void Save() const {
        if (path.empty()) return;
        json root;
        root["version"] = 1;
        root["seed"] = ToHex(encrypted_seed);
        root["birth_height"] = birth_height;
        root["accounts"] = json::array();
        for (const auto& account : accounts) {
            root["accounts"].push_back({
                {"label", account.label}, {"scheme", account.scheme_name},
                {"id", account.account_id}, {"highest", account.highest_derived},
                {"gap", account.gap_limit}});
        }
        root["metadata"] = metadata;
        std::ofstream file(path, std::ios::trunc);
        if (file) file << root.dump(2);
    }

    void Load() {
        if (path.empty()) return;
        std::ifstream file(path);
        if (!file) return;
        json root = json::parse(file, nullptr, false);
        if (root.is_discarded() || root.value("version", 0) != 1) return;
        const auto seed = FromHex(root.value("seed", ""));
        if (!seed.has_value() || seed->empty()) return;
        encrypted_seed = *seed;
        has_seed = true;
        birth_height = root.value("birth_height", 0U);
        if (root.contains("accounts") && root["accounts"].is_array()) {
            for (const auto& item : root["accounts"]) {
                Account account;
                account.label = item.value("label", "");
                account.scheme_name = item.value("scheme", "");
                account.account_id = item.value("id", 0U);
                account.highest_derived = item.value("highest", 0U);
                account.gap_limit = item.value("gap", size_t{20});
                accounts.push_back(std::move(account));
            }
        }
        if (root.contains("metadata") && root["metadata"].is_object()) {
            metadata = root["metadata"].get<std::unordered_map<std::string, std::string>>();
        }
    }
};

WalletDb::WalletDb(const std::string& path, std::string_view password)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    impl_->Load();
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
    impl_->Save();
}

std::optional<ByteVec> WalletDb::LoadSeed(std::string_view password) const {
    if (!impl_->has_seed) return std::nullopt;
    return SimpleDecrypt(impl_->encrypted_seed, password);
}

void WalletDb::SetBirthHeight(uint32_t height) {
    impl_->birth_height = height;
    impl_->Save();
}

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
    impl_->Save();
}

void WalletDb::UpdateAccount(const Account& account) {
    for (auto& a : impl_->accounts) {
        if (a.account_id == account.account_id &&
            a.scheme_name == account.scheme_name) {
            a = account;
            impl_->Save();
            return;
        }
    }
}

void WalletDb::RemoveAccount(size_t index) {
    if (index >= impl_->accounts.size()) return;
    impl_->accounts.erase(impl_->accounts.begin() + static_cast<ptrdiff_t>(index));
    impl_->Save();
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
    impl_->Save();
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
    impl_->Save();
}

std::optional<std::string> WalletDb::GetMetadata(const std::string& key) const {
    auto it = impl_->metadata.find(key);
    if (it == impl_->metadata.end()) return std::nullopt;
    return it->second;
}

}  // namespace amarian::wallet
