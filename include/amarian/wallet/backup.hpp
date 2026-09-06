#pragma once

/// \file
/// Backup and recovery: seed mnemonic generation and metadata serialisation.
///
/// The backup consists of two parts:
///   1. Recovery phrase (BIP-39 mnemonic encoding of the 256-bit master seed)
///   2. Metadata (schemes in use, account list, highest index per account, labels, birth height)
///
/// The phrase alone is not sufficient — without metadata, recovery requires scanning
/// a bounded gap of indices across every registered scheme. The birth height limits
/// that scan to a reasonable range.

#include <amarian/wallet/seed.hpp>
#include <amarian/wallet/wallet.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amarian::wallet {

/// The number of words in a 256-bit seed mnemonic (BIP-39: 24 words).
inline constexpr size_t MNEMONIC_WORD_COUNT = 24;

/// The index of the wordlist. BIP-39 English wordlist has 2048 entries.
inline constexpr size_t BIP39_WORDLIST_SIZE = 2048;

/// Generates the BIP-39 mnemonic from a master seed.
///
/// The seed is 32 bytes = 256 bits.
/// BIP-39: 256 bits -> 24 words + 8 checksum bits = 24 words.
[[nodiscard]] std::vector<std::string> GenerateMnemonic(const MasterSeed& seed);

/// Recovers a master seed from its BIP-39 mnemonic.
[[nodiscard]] std::optional<MasterSeed> SeedFromMnemonic(const std::vector<std::string>& words);

/// Serialises wallet metadata to a portable byte vector.
///
/// The metadata contains: birth height, account list (scheme, account_id,
/// highest index, label, gap limit), and any custom key-value pairs.
[[nodiscard]] ByteVec SerialiseMetadata(const WalletDb& db);

/// Deserialises wallet metadata from bytes, updating `db`.
[[nodiscard]] bool DeserialiseMetadata(ByteSpan data, WalletDb& db);

/// Exports a full backup: mnemonic + metadata.
struct WalletBackup {
    std::vector<std::string> mnemonic;
    ByteVec metadata;
};
[[nodiscard]] WalletBackup ExportBackup(const MasterSeed& seed, const WalletDb& db);

/// The BIP-39 English wordlist.
[[nodiscard]] std::string_view Bip39Word(size_t index);

}  // namespace amarian::wallet