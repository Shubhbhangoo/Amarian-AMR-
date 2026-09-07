#pragma once

/// \file
/// The top-level Wallet interface: balance, receive, send, list transactions, recovery.
///
/// This is the API that the node's RPC layer and the CLI tool call.
/// Everything a user can do with their wallet goes through here.

#include <amarian/consensus/params.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/wallet/backup.hpp>
#include <amarian/wallet/coinselection.hpp>
#include <amarian/wallet/fees.hpp>
#include <amarian/wallet/seed.hpp>
#include <amarian/wallet/wallet.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amarian::wallet {

/// Wallet balance information.
struct Balance {
    /// Confirmed balance (in facets).
    int64_t confirmed = 0;
    /// Unconfirmed (pending) balance.
    int64_t pending = 0;
    /// Total = confirmed + pending.
    [[nodiscard]] int64_t Total() const noexcept { return confirmed + pending; }
};

/// Information about one receive address.
struct AddressInfo {
    std::string address;
    Lock lock;
    uint32_t account_id;
    uint32_t index;
};

/// The result of a send operation.
struct SendResult {
    Hash256 txid;
    int64_t fee;
    std::string raw_tx_hex;
};

/// The main wallet interface.
///
/// Thread-compatible: all calls are made from one thread.
class Wallet {
public:
    ~Wallet();
    Wallet(Wallet&&) noexcept;
    Wallet& operator=(Wallet&&) noexcept;
    Wallet(const Wallet&) = delete;
    Wallet& operator=(const Wallet&) = delete;

    /// Creates a new wallet with a fresh seed.
    /// If `password` is provided, the seed is encrypted at rest.
    Wallet(std::string_view db_path, std::string_view password,
           const ChainParams& params);

    /// Opens an existing wallet.
    /// Returns nullopt if the wallet file does not exist or the password is wrong.
    [[nodiscard]] static std::optional<Wallet> Open(std::string_view db_path,
                                                     std::string_view password,
                                                     const ChainParams& params);

    /// Returns the wallet's seed.
    [[nodiscard]] const MasterSeed& GetSeed() const noexcept { return seed_; }

    // --- Balance ---
    [[nodiscard]] Balance GetBalance() const;
    void SetBalance(const Balance& balance) { balance_ = balance; }

    /// Rebuilds wallet-owned UTXOs from the active chain, starting at birth height.
    void Rescan(const std::vector<std::pair<Block, uint32_t>>& blocks);

    // --- Receive ---
    /// Generates a fresh receive address for the given account.
    [[nodiscard]] AddressInfo GetNewAddress(uint32_t account_id = 0);

    /// Lists all addresses for the given account within the gap limit.
    [[nodiscard]] std::vector<AddressInfo> ListAddresses(uint32_t account_id = 0) const;

    // --- Send ---
    /// Creates, signs, and returns a transaction sending `amount` facets to `address`.
    [[nodiscard]] std::optional<SendResult>
    Send(std::string_view address, int64_t amount, int64_t fee_rate = 0);

    /// Creates, signs, and returns a transaction sending `amount` facets to a `lock`.
    [[nodiscard]] std::optional<SendResult>
    SendToLock(const Lock& lock, int64_t amount, int64_t fee_rate = 0);

    // --- Transactions ---
    [[nodiscard]] std::vector<StoredTransaction> ListTransactions() const;
    void AddTransaction(const StoredTransaction& tx);

    // --- Backup ---
    [[nodiscard]] WalletBackup ExportBackup() const;
    [[nodiscard]] bool RestoreFromBackup(const WalletBackup& backup);

    // --- Settings ---
    void SetFeePreference(std::string_view preference) { fee_preference_ = preference; }
    [[nodiscard]] std::string_view FeePreference() const noexcept { return fee_preference_; }

    // --- Node connection ---
    void SetNodeUrl(std::string_view url) { node_url_ = url; }
    [[nodiscard]] std::string_view NodeUrl() const noexcept { return node_url_; }

private:
    MasterSeed seed_;
    WalletDb db_;
    Balance balance_;
    std::string fee_preference_ = "normal";
    std::string node_url_;
    const ChainParams* params_ = nullptr;
    mutable uint32_t next_index_by_account_[64] = {};
    std::vector<UtxoEntry> utxos_;
    uint32_t tip_height_ = 0;

    /// Derives a lock from a key at (account, index).
    [[nodiscard]] Lock DeriveLock(uint32_t account_id, uint32_t index) const;
};

}  // namespace amarian::wallet
