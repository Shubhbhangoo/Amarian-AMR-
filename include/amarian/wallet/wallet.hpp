#pragma once

/// \file
/// The wallet's internal data model: accounts, transactions, and a persistent store.
///
/// The wallet stores:
///   - Master seed (encrypted at rest under a passphrase)
///   - Account metadata (which schemes, highest index reached, labels)
///   - Stored transactions (confirmed and pending, with their outputs)
///   - Birth height (where to start scanning)

#include <amarian/consensus/params.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amarian::wallet {

/// Maximum number of accounts the wallet can manage.
inline constexpr size_t MAX_ACCOUNTS = 64;

/// One account: a set of derived keys under a single scheme.
struct Account {
    /// Human-readable label, e.g. "Everyday Spending"
    std::string label;

    /// The scheme name from the registry, e.g. "schnorr-secp256k1"
    std::string scheme_name;

    /// The numeric account identifier, used in derivation.
    uint32_t account_id = 0;

    /// The highest index derived so far for this account.
    /// The next receive address uses index = highest_index + 1.
    uint32_t highest_derived = 0;

    /// The gap limit: how many unused addresses ahead to pre-generate.
    size_t gap_limit = 20;
};

/// One stored transaction, as the wallet sees it.
struct StoredTransaction {
    Hash256 txid;
    int32_t height = -1;  // -1 = unconfirmed (mempool)
    int64_t timestamp = 0;
    /// Total value received by this wallet in this tx (in facets).
    int64_t received = 0;
    /// Total value sent by this wallet in this tx (in facets).
    int64_t sent = 0;
    /// Fee paid (in facets). 0 for received-only transactions.
    int64_t fee = 0;
    /// Raw transaction bytes (for re-broadcast or inspection).
    ByteVec raw_tx;
};

/// Wallet database interface.
///
/// The wallet is stored as a single file (or in-memory for tests).
/// The seed is encrypted at rest.
/// Metadata and transactions are stored in a simple portable format.
class WalletDb {
public:
    /// Opens or creates a wallet database at `path`.
    /// `password` is used to encrypt/decrypt the seed at rest.
    WalletDb(const std::string& path, std::string_view password);

    /// Opens an in-memory wallet (for tests).
    WalletDb();

    ~WalletDb();

    WalletDb(const WalletDb&) = delete;
    WalletDb(WalletDb&&) noexcept;
    WalletDb& operator=(WalletDb&&) noexcept;
    WalletDb& operator=(const WalletDb&) = delete;

    // --- Seed ---
    [[nodiscard]] bool HasSeed() const noexcept;
    void StoreSeed(ByteSpan seed, std::string_view password);
    [[nodiscard]] std::optional<ByteVec> LoadSeed(std::string_view password) const;

    // --- Birth height ---
    void SetBirthHeight(uint32_t height);
    [[nodiscard]] uint32_t BirthHeight() const noexcept;

    // --- Accounts ---
    [[nodiscard]] size_t AccountCount() const noexcept;
    [[nodiscard]] std::optional<Account> GetAccount(size_t index) const;
    void AddAccount(const Account& account);
    void UpdateAccount(const Account& account);
    void RemoveAccount(size_t index);

    // --- Transactions ---
    [[nodiscard]] size_t TxCount() const noexcept;
    [[nodiscard]] std::optional<StoredTransaction> GetTx(const Hash256& txid) const;
    void StoreTx(const StoredTransaction& tx);
    [[nodiscard]] std::vector<StoredTransaction> ListTxs() const;

    // --- Metadata ---
    void SetMetadata(const std::string& key, const std::string& value);
    [[nodiscard]] std::optional<std::string> GetMetadata(const std::string& key) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace amarian::wallet